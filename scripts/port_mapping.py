"""Opens the game's UDP port on the home router for online play (run.py, with a shadNet server set), so other
players reach this PC directly instead of through the server's relay: UPnP (the router's AddPortMapping), else
NAT-PMP. Best effort: a router with neither, or with them turned off, leaves everything as it was, and a match
still finds its way (direct probes, then the relay). MT_PORT_MAPPING=0 turns it off.

The mapping lasts LEASE seconds and is renewed while the game runs; it is removed when the game ends.
"""
import ipaddress
import re
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from xml.sax.saxutils import escape
import xml.etree.ElementTree as ET

LEASE = 3600
DESCRIPTION = 'Minus Two online play'
SSDP_ADDRESS = ('239.255.255.250', 1900)
SEARCH_TARGETS = ('urn:schemas-upnp-org:device:InternetGatewayDevice:1',
                  'urn:schemas-upnp-org:device:InternetGatewayDevice:2')
WAN_SERVICES = ('urn:schemas-upnp-org:service:WANIPConnection:2', 'urn:schemas-upnp-org:service:WANIPConnection:1',
                'urn:schemas-upnp-org:service:WANPPPConnection:1')
NATPMP_PORT = 5351
MAX_REPLY = 65536  # bytes read from a router's answer: its description and SOAP replies are a few KiB
# The router's own requests go straight to it, never through a proxy from the environment.
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


class UpnpError(Exception):
    def __init__(self, code, description):
        super().__init__(f'UPnP error {code} ({description})')
        self.code = code


def free_udp_port():
    """A UDP port free on this PC now (the game binds it when VS Mode opens)."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(('0.0.0.0', 0))
        return s.getsockname()[1]


def local_address(towards):
    """This PC's address on the network that leads to `towards` (an address; no packet is sent)."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect((towards, 9))
            return s.getsockname()[0]
    except OSError:
        return None


def server_address(server):
    """The IPv4 address of MT_SHADNET_SERVER (host or host:port), or a public one when it cannot be resolved."""
    host = (server or '').strip()
    if host.count(':') == 1:
        host = host.split(':')[0]
    try:
        return socket.gethostbyname(host) if host else '8.8.8.8'
    except OSError:
        return '8.8.8.8'


def local_url(url):
    """A router's URL: plain http to an address on the local network (SSDP answers come from any device there)."""
    try:
        parts = urllib.parse.urlsplit(url)
        host = ipaddress.ip_address(parts.hostname or '')
    except ValueError:
        return False
    return parts.scheme == 'http' and (host.is_private or host.is_link_local)


# ---- UPnP --------------------------------------------------------------------------------------------
def discover(interface, timeout=2.0):
    """The description URLs of the gateways that answer an SSDP search sent from `interface`."""
    found = []
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP) as s:
            s.bind((interface, 0))
            s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(interface))
            s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
            s.settimeout(0.2)
            for target in SEARCH_TARGETS:
                s.sendto((f'M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: "ssdp:discover"\r\n'
                          f'MX: 2\r\nST: {target}\r\n\r\n').encode('ascii'), SSDP_ADDRESS)
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                try:
                    data, _ = s.recvfrom(4096)
                except socket.timeout:
                    continue
                match = re.search(rb'^location:[ \t]*(\S+)', data, re.IGNORECASE | re.MULTILINE)
                url = match.group(1).decode('ascii', 'replace') if match else ''
                if local_url(url) and url not in found:
                    found.append(url)
                    end = min(end, time.monotonic() + 0.3)  # others answer about as fast
    except OSError:
        pass
    return found


def wan_service(description, location):
    """(control URL, service type) of the WAN connection service in a gateway's description XML, or None."""
    root = ET.fromstring(description)
    def local(tag):
        return tag.rsplit('}', 1)[-1]
    base = next((e.text.strip() for e in root.iter() if local(e.tag) == 'URLBase' and e.text), '') or location
    services = {}
    for element in root.iter():
        if local(element.tag) != 'service':
            continue
        fields = {local(child.tag): (child.text or '').strip() for child in element}
        if fields.get('serviceType') in WAN_SERVICES and fields.get('controlURL'):
            services.setdefault(fields['serviceType'], urllib.parse.urljoin(base, fields['controlURL']))
    for kind in WAN_SERVICES:
        if kind in services and local_url(services[kind]):
            return services[kind], kind
    return None


def soap(control, service, action, arguments, timeout=3):
    """Calls a UPnP action; its reply's fields by name. A refusal raises UpnpError with the router's code."""
    body = ''.join(f'<{name}>{escape(str(value))}</{name}>' for name, value in arguments)
    envelope = ('<?xml version="1.0"?>\r\n<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
                's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>'
                f'<u:{action} xmlns:u="{service}">{body}</u:{action}></s:Body></s:Envelope>\r\n')
    request = urllib.request.Request(control, data=envelope.encode('utf-8'), method='POST', headers={
        'Content-Type': 'text/xml; charset="utf-8"', 'SOAPAction': f'"{service}#{action}"'})
    try:
        with OPENER.open(request, timeout=timeout) as reply:
            text = reply.read(MAX_REPLY).decode('utf-8', 'replace')
    except urllib.error.HTTPError as error:
        text = error.read(MAX_REPLY).decode('utf-8', 'replace')
        code = re.search(r'<errorCode>\s*(\d+)\s*</errorCode>', text)
        description = re.search(r'<errorDescription>\s*([^<]*)</errorDescription>', text)
        raise UpnpError(int(code.group(1)) if code else error.code,
                        description.group(1).strip() if description else error.reason) from None
    return dict(re.findall(r'<(?:\w+:)?(New\w+)>([^<]*)</', text))


class Upnp:
    """A port mapping through a gateway's WAN connection service."""
    name = 'UPnP'

    def __init__(self, control, service):
        self.control, self.service = control, service

    def external_address(self):
        try:
            return soap(self.control, self.service, 'GetExternalIPAddress', []).get('NewExternalIPAddress')
        except (OSError, UpnpError):
            return None

    def add(self, port, client, lease):
        """Maps the external UDP port to client:port; the lease it got (0: until removed)."""
        arguments = [('NewRemoteHost', ''), ('NewExternalPort', port), ('NewProtocol', 'UDP'), ('NewInternalPort', port),
                     ('NewInternalClient', client), ('NewEnabled', 1), ('NewPortMappingDescription', DESCRIPTION),
                     ('NewLeaseDuration', lease)]
        try:
            soap(self.control, self.service, 'AddPortMapping', arguments)
            return lease
        except UpnpError as error:
            if error.code != 725:  # OnlyPermanentLeasesSupported: it takes only a mapping until removed
                raise
            arguments[-1] = ('NewLeaseDuration', 0)
            soap(self.control, self.service, 'AddPortMapping', arguments)
            return 0

    def remove(self, port):
        soap(self.control, self.service, 'DeletePortMapping',
             [('NewRemoteHost', ''), ('NewExternalPort', port), ('NewProtocol', 'UDP')])


def find_upnp(interface, timeout=2.0):
    for location in discover(interface, timeout):
        try:
            with OPENER.open(location, timeout=3) as reply:
                found = wan_service(reply.read(MAX_REPLY), location)
        except (OSError, ValueError, ET.ParseError):
            continue
        if found:
            return Upnp(*found)
    return None


# ---- NAT-PMP (RFC 6886) --------------------------------------------------------------------------------
def default_gateway():
    """The IPv4 default gateway's address, or None."""
    try:
        if sys.platform.startswith('linux'):
            with open('/proc/net/route') as routes:
                for line in routes.read().splitlines()[1:]:
                    fields = line.split()
                    if len(fields) > 2 and fields[1] == '00000000' and fields[2] != '00000000':
                        return socket.inet_ntoa(struct.pack('<I', int(fields[2], 16)))
        elif sys.platform == 'darwin':
            out = subprocess.run(['route', '-n', 'get', 'default'], capture_output=True, text=True, timeout=3).stdout
            match = re.search(r'gateway:\s*([\d.]+)', out)
            return match.group(1) if match else None
        elif sys.platform == 'win32':
            import ctypes
            from ctypes import wintypes

            class Row(ctypes.Structure):  # MIB_IPFORWARDROW
                _fields_ = [(name, wintypes.DWORD) for name in (
                    'dest', 'mask', 'policy', 'next_hop', 'if_index', 'type', 'proto', 'age', 'next_hop_as',
                    'metric1', 'metric2', 'metric3', 'metric4', 'metric5')]
            row = Row()
            destination = struct.unpack('<I', socket.inet_aton('8.8.8.8'))[0]
            if ctypes.windll.iphlpapi.GetBestRoute(destination, 0, ctypes.byref(row)) == 0 and row.next_hop:
                return socket.inet_ntoa(struct.pack('<I', row.next_hop))
    except (OSError, ValueError, subprocess.SubprocessError, AttributeError):
        pass
    return None


def natpmp_request(gateway, request, opcode, port=NATPMP_PORT, tries=4):
    """Sends a NAT-PMP request, waiting 250 ms and doubling as RFC 6886 says; the answer to `opcode` or None."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        wait = 0.25
        for _ in range(tries):
            s.sendto(request, (gateway, port))
            s.settimeout(wait)
            try:
                while True:
                    data, sender = s.recvfrom(64)
                    if sender[0] == gateway and len(data) >= 8 and data[0] == 0 and data[1] == 128 + opcode:
                        return data
            except socket.timeout:
                wait *= 2
            except OSError:
                return None
    return None


class NatPmp:
    name = 'NAT-PMP'

    def __init__(self, gateway, port=NATPMP_PORT):
        self.gateway, self.port = gateway, port

    def external_address(self):
        data = natpmp_request(self.gateway, struct.pack('!BB', 0, 0), 0, self.port)
        if data and len(data) >= 12 and struct.unpack('!H', data[2:4])[0] == 0:
            return socket.inet_ntoa(data[8:12])
        return None

    def add(self, port, client, lease):
        """Maps the UDP port (NAT-PMP maps the requesting PC: `client` is not sent); the lease it got. The
        router may pick another external port: the game's packets then leave through it, and the server's
        STUN tells the other players that one."""
        del client
        data = natpmp_request(self.gateway, struct.pack('!BBHHHI', 0, 1, 0, port, port, lease), 1, self.port)
        if not data or len(data) < 16:
            raise OSError('no NAT-PMP answer')
        result = struct.unpack('!H', data[2:4])[0]
        if result:
            raise UpnpError(result, 'NAT-PMP refused the mapping')
        return struct.unpack('!I', data[12:16])[0]

    def remove(self, port):
        natpmp_request(self.gateway, struct.pack('!BBHHHI', 0, 1, 0, port, 0, 0), 1, self.port)


# ---- the game's mapping -----------------------------------------------------------------------------------
def private(address):
    """An address behind another NAT (private or the providers' shared range): a mapping on this router
    then opens nothing to the internet."""
    try:
        ip = ipaddress.ip_address(address)
    except ValueError:
        return False
    return ip.is_private or ip in ipaddress.ip_network('100.64.0.0/10')


class PortMapping:
    """Opens `port` in a thread of its own (discovery takes up to a few seconds and the game starts meanwhile),
    renews it while the game runs, removes it in close()."""

    def __init__(self, port, towards, log=print, finders=None):
        self.port, self.towards, self.log = port, towards, log
        self.finders = finders
        self.router = None
        self.lease = LEASE
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run, name='PortMapping', daemon=True)

    def start(self):
        self.thread.start()
        return self

    def find(self, client):
        def natpmp():
            gateway = default_gateway()
            return NatPmp(gateway) if gateway else None
        for finder in self.finders or (lambda: find_upnp(client), natpmp):
            router = finder()
            if not router:
                continue
            try:
                self.lease = router.add(self.port, client, LEASE)
                return router
            except (OSError, UpnpError) as error:
                if isinstance(error, UpnpError) and error.code == 718:
                    self.log(f'Online: the router already forwards UDP port {self.port} to another device')
                    return None
                self.log(f'Online: {router.name} did not open UDP port {self.port}: {error}')
        return None

    def run(self):
        client = local_address(self.towards)
        router = self.find(client) if client else None
        if not router:
            self.log(f'Online: the router did not open UDP port {self.port} (no UPnP or NAT-PMP answer, or turned off '
                     'in the router): players who cannot reach this PC directly play through the server\'s relay')
            return
        self.router = router
        if self.stop.is_set():  # the game ended while the router was being asked
            self.close_router()
            return
        external = router.external_address()
        where = f'; its internet address {external}' if external else ''
        self.log(f'Online: UDP port {self.port} opened on the router ({router.name}{where}): other players can reach '
                 'this PC directly')
        if external and private(external):
            self.log(f'Online: but {external} is not an internet address: another router or the internet provider\'s '
                     'NAT is in front of this one, so the opened port may not reach this PC')
        while self.lease and not self.stop.wait(max(60, self.lease // 2)):
            try:
                self.lease = router.add(self.port, client, LEASE)
            except (OSError, UpnpError) as error:
                self.log(f'Online: renewing UDP port {self.port} on the router failed: {error}')

    def close_router(self):
        router, self.router = self.router, None
        if router:
            try:
                router.remove(self.port)
            except (OSError, UpnpError):
                pass  # its lease runs out by itself

    def close(self):
        """The game ended: the port is closed on the router again."""
        self.stop.set()
        if self.thread.is_alive():
            self.thread.join(timeout=5)
        self.close_router()
