"""scripts/port_mapping.py: the router's UDP port for online play, against a fake UPnP gateway (a local HTTP
server) and a fake NAT-PMP router (a local UDP socket)."""
import http.server
import socket
import struct
import threading
import unittest

from paths import ROOT  # noqa: F401 (puts scripts/ on the path)
import port_mapping  # noqa: E402

DESCRIPTION = b'''<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
 <device><deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>
  <deviceList><device><deviceList><device>
   <serviceList>
    <service><serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1</serviceType>
     <controlURL>/ctl/PPP</controlURL></service>
    <service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>
     <controlURL>/ctl/IPConn</controlURL></service>
    <service><serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>
     <controlURL>/ctl/L3F</controlURL></service>
   </serviceList></device></deviceList></device></deviceList>
 </device>
</root>'''


class FakeGateway(http.server.BaseHTTPRequestHandler):
    """A gateway's WANIPConnection: AddPortMapping (refused with 725 for a lease that is not 0 while
    permanent_only), DeletePortMapping, GetExternalIPAddress. Calls are recorded."""
    calls, permanent_only, conflict = [], False, False

    def do_POST(self):
        body = self.rfile.read(int(self.headers['Content-Length'])).decode()
        action = self.headers['SOAPAction'].strip('"').split('#')[1]
        FakeGateway.calls.append((action, body))
        if action == 'AddPortMapping' and FakeGateway.conflict:
            return self.fault(718, 'ConflictInMappingEntry')
        if action == 'AddPortMapping' and FakeGateway.permanent_only and '<NewLeaseDuration>0<' not in body:
            return self.fault(725, 'OnlyPermanentLeasesSupported')
        reply = ('<NewExternalIPAddress>203.0.113.7</NewExternalIPAddress>'
                 if action == 'GetExternalIPAddress' else '')
        self.answer(200, f'<s:Envelope><s:Body><u:{action}Response>{reply}</u:{action}Response></s:Body></s:Envelope>')

    def fault(self, code, text):
        self.answer(500, f'<s:Envelope><s:Body><s:Fault><detail><UPnPError><errorCode>{code}</errorCode>'
                         f'<errorDescription>{text}</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>')

    def answer(self, status, text):
        data = text.encode()
        self.send_response(status)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


class UpnpTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = http.server.HTTPServer(('127.0.0.1', 0), FakeGateway)
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        cls.control = f'http://127.0.0.1:{cls.server.server_port}/ctl/IPConn'

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()

    def setUp(self):
        FakeGateway.calls, FakeGateway.permanent_only, FakeGateway.conflict = [], False, False

    def test_the_wan_ip_service_is_preferred_and_its_url_made_absolute(self):
        found = port_mapping.wan_service(DESCRIPTION, 'http://192.168.1.1:5000/rootDesc.xml')
        self.assertEqual(found, ('http://192.168.1.1:5000/ctl/IPConn', 'urn:schemas-upnp-org:service:WANIPConnection:1'))

    def test_only_plain_http_on_the_local_network(self):
        for url in ('http://192.168.1.1:5000/x', 'http://10.0.0.1/x', 'http://169.254.1.1/x', 'http://127.0.0.1:80/x'):
            self.assertTrue(port_mapping.local_url(url), url)
        for url in ('https://192.168.1.1/x', 'http://8.8.8.8/x', 'http://router.example/x', 'file:///etc/passwd', ''):
            self.assertFalse(port_mapping.local_url(url), url)
        # a description that sends its control URL elsewhere is not used
        moved = DESCRIPTION.replace(b'/ctl/IPConn', b'http://8.8.8.8/ctl').replace(b'/ctl/PPP', b'http://8.8.8.8/p')
        self.assertIsNone(port_mapping.wan_service(moved, 'http://192.168.1.1:5000/rootDesc.xml'))

    def test_add_external_address_and_remove(self):
        router = port_mapping.Upnp(self.control, 'urn:schemas-upnp-org:service:WANIPConnection:1')
        self.assertEqual(router.add(40000, '192.168.1.20', 3600), 3600)
        action, body = FakeGateway.calls[-1]
        self.assertEqual(action, 'AddPortMapping')
        for field in ('<NewExternalPort>40000<', '<NewInternalPort>40000<', '<NewProtocol>UDP<',
                      '<NewInternalClient>192.168.1.20<', '<NewLeaseDuration>3600<', '<NewEnabled>1<'):
            self.assertIn(field, body)
        self.assertEqual(router.external_address(), '203.0.113.7')
        router.remove(40000)
        self.assertEqual(FakeGateway.calls[-1][0], 'DeletePortMapping')

    def test_a_router_that_takes_only_permanent_mappings(self):
        FakeGateway.permanent_only = True
        router = port_mapping.Upnp(self.control, 'urn:schemas-upnp-org:service:WANIPConnection:1')
        self.assertEqual(router.add(40000, '192.168.1.20', 3600), 0)
        self.assertEqual([c[0] for c in FakeGateway.calls], ['AddPortMapping', 'AddPortMapping'])

    def test_a_refusal_carries_the_router_code(self):
        FakeGateway.conflict = True
        router = port_mapping.Upnp(self.control, 'urn:schemas-upnp-org:service:WANIPConnection:1')
        with self.assertRaises(port_mapping.UpnpError) as raised:
            router.add(40000, '192.168.1.20', 3600)
        self.assertEqual(raised.exception.code, 718)


class FakeNatPmp:
    """A NAT-PMP router on loopback: external address 203.0.113.9, maps UDP ports as asked."""
    def __init__(self):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(('127.0.0.1', 0))
        self.port = self.socket.getsockname()[1]
        self.requests = []
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            try:
                data, sender = self.socket.recvfrom(64)
            except OSError:
                return
            self.requests.append(data)
            if data[1] == 0:
                self.socket.sendto(struct.pack('!BBHI4s', 0, 128, 0, 1, socket.inet_aton('203.0.113.9')), sender)
            elif data[1] == 1:
                _, _, _, internal, external, lease = struct.unpack('!BBHHHI', data)
                self.socket.sendto(struct.pack('!BBHIHHI', 0, 129, 0, 1, internal, external, lease), sender)


class NatPmpTest(unittest.TestCase):
    def test_map_external_address_and_remove(self):
        fake = FakeNatPmp()
        router = port_mapping.NatPmp('127.0.0.1', fake.port)
        self.assertEqual(router.add(40001, None, 3600), 3600)
        self.assertEqual(struct.unpack('!BBHHHI', fake.requests[-1]), (0, 1, 0, 40001, 40001, 3600))
        self.assertEqual(router.external_address(), '203.0.113.9')
        router.remove(40001)
        self.assertEqual(struct.unpack('!BBHHHI', fake.requests[-1])[4:], (0, 0))  # lifetime 0: removed
        fake.socket.close()

    def test_no_router_answers(self):
        silent = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        silent.bind(('127.0.0.1', 0))
        self.assertIsNone(port_mapping.natpmp_request('127.0.0.1', b'\0\0', 0, silent.getsockname()[1], tries=2))
        silent.close()


class PortMappingTest(unittest.TestCase):
    class Router:
        name = 'Fake'

        def __init__(self, fail=None):
            self.fail, self.added, self.removed = fail, [], []

        def add(self, port, client, lease):
            if self.fail:
                raise self.fail
            self.added.append((port, client, lease))
            return lease

        def remove(self, port):
            self.removed.append(port)

        def external_address(self):
            return '100.64.3.4'  # behind the provider's NAT

    def run_mapping(self, *finders):
        lines = []
        mapping = port_mapping.PortMapping(40002, '127.0.0.1', log=lines.append, finders=finders).start()
        mapping.thread.join(timeout=5)
        return mapping, lines

    def test_opened_said_and_removed_at_the_end(self):
        router = self.Router()
        mapping, lines = self.run_mapping(lambda: None, lambda: router)  # no UPnP, then NAT-PMP answers
        self.assertEqual(router.added, [(40002, '127.0.0.1', port_mapping.LEASE)])
        self.assertTrue(any('UDP port 40002 opened on the router (Fake; its internet address 100.64.3.4)' in l
                            for l in lines), lines)
        self.assertTrue(any('not an internet address' in l for l in lines), lines)  # double NAT is said
        mapping.close()
        self.assertEqual(router.removed, [40002])

    def test_nothing_opened_is_said(self):
        mapping, lines = self.run_mapping(lambda: None, lambda: None)
        self.assertIn('did not open UDP port 40002', lines[-1])
        mapping.close()

    def test_a_port_forwarded_elsewhere_is_said(self):
        mapping, lines = self.run_mapping(lambda: self.Router(port_mapping.UpnpError(718, 'ConflictInMappingEntry')))
        self.assertTrue(any('already forwards UDP port 40002' in l for l in lines), lines)
        mapping.close()

    def test_addresses(self):
        self.assertTrue(port_mapping.private('192.168.50.1') and port_mapping.private('100.64.0.9'))
        self.assertFalse(port_mapping.private('104.34.13.53'))
        self.assertEqual(port_mapping.server_address('localhost:31313'), '127.0.0.1')
        self.assertEqual(port_mapping.local_address('127.0.0.1'), '127.0.0.1')
        port = port_mapping.free_udp_port()
        self.assertTrue(0 < port < 65536)


if __name__ == '__main__':
    unittest.main()
