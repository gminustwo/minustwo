/* P2P sockets and Rudp for online play through shadNet (runtime_np.c with MT_SHADNET_SERVER).
 *
 * On a PS4 the players' consoles reach each other directly: NpSignaling finds a path through their
 * routers, the game's P2P UDP socket (SCE_NET_SOCK_DGRAM_P2P, one system port shared by "virtual ports")
 * carries datagrams, and Rudp runs reliable connections over it. Here both ends are this port, so the
 * wire format is our own:
 *
 *  - One real UDP socket (MT_P2P_PORT, or any free port) carries everything. Its first packets go to the
 *    shadNet server's STUN service, which records this player's public address for the others.
 *  - Each player tells the room its local address in a shadNet room message; the other players try the
 *    local and the public address until one answers (hole punching). The game is told a member is
 *    reachable (signaling ESTABLISHED) once a probe got through both ways.
 *  - The game sees member N at the virtual address 10.77.0.N; its packets to that address go to the
 *    endpoint that answered.
 *  - Rudp contexts are reliable, ordered message streams: sequence numbers, acknowledgements and resends.
 *    Both players call sceRudpInitiate (simultaneous open), as the game does.
 *
 * Datagram header (little-endian): "MT", version, type, sender's room member id, source and destination
 * virtual port, sequence, acknowledgement, payload length. */
#define _GNU_SOURCE
#include "runtime.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET UdpSocket;
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int UdpSocket;
#define INVALID_SOCKET (-1)
#define close_socket close
#endif

enum {
    P2P_VERSION = MT_ONLINE_PROTOCOL,
    T_PROBE = 1, T_PROBE_ACK = 2, T_RAW = 3, T_SYN = 4, T_DATA = 5, T_ACK = 6, T_FIN = 7, T_BYE = 8,
    MAX_PEERS = 8, MAX_SOCKETS = 16, MAX_CONTEXTS = 16, MAX_DATAGRAM = 9000, MAX_QUEUED = 256,
    SOCKET_ID_BASE = 0x7000, RESEND_MS = 120, CONNECT_TIMEOUT_MS = 20000, PROBE_MS = 200, STUN_MS = 15000,
    PS4_AF_INET = 2, PS4_SOCK_DGRAM_P2P = 6, PS4_SOCK_STREAM_P2P = 10, PS4_SOL_SOCKET = 0xFFFF, PS4_SO_NBIO = 0x1200,
    PS4_MSG_DONTWAIT = 0x80, ROOM_MESSAGE_COMMAND = 115, CAST_BROADCAST = 1,
};
#define NET_ERROR(e) ((int32_t)(0x80410100u | (uint32_t)(e)))
enum { NET_EBADF = 9, NET_EINVAL = 22, NET_EWOULDBLOCK = 35, NET_EADDRINUSE = 48, NET_ENETUNREACH = 51, NET_EPROTONOSUPPORT = 43 };
#define RUDP_ERROR(e) ((int32_t)(0x80770000u | (uint32_t)(e)))
enum {
    RUDP_INVALID_CONTEXT = 0x03, RUDP_INVALID_ARGUMENT = 0x04, RUDP_CONN_RESET = 0x09, RUDP_CONN_TIMEOUT = 0x0B,
    RUDP_NOT_BOUND = 0x13, RUDP_WOULDBLOCK = 0x16, RUDP_TOO_MANY_CONTEXTS = 0x20,
    RUDP_EVENT_CLOSED = 1, RUDP_EVENT_ESTABLISHED = 2, RUDP_EVENT_ERROR = 3, RUDP_EVENT_WRITABLE = 4,
    RUDP_EVENT_READABLE = 5, RUDP_EVENT_FLUSHED = 6,
    RUDP_OPTION_NONBLOCK = 7, RUDP_OPTION_CONNECTION_TIMEOUT = 9, RUDP_MSG_DONTWAIT = 0x01,
};

#pragma pack(push, 1)
typedef struct {
    char magic[2]; uint8_t version, type; uint16_t member, src_vport, dst_vport; uint32_t seq, ack; uint16_t length;
} Header;
#pragma pack(pop)
_Static_assert(sizeof(Header) == 20, "P2P header");
/* The PS4's sockaddr_in, with the P2P virtual port. */
typedef struct { uint8_t len, family; uint16_t port; uint32_t addr; uint16_t vport; uint8_t zero[6]; } Ps4SockaddrIn;
_Static_assert(sizeof(Ps4SockaddrIn) == 16, "SceNetSockaddrIn");

typedef struct Packet { struct Packet *next; uint32_t seq; uint16_t size, vport; uint32_t from; uint64_t sent_ms; uint8_t data[]; } Packet;

typedef struct {
    int used; uint16_t member; char npid[17];
    uint32_t candidate_ip[2]; uint16_t candidate_port[2]; /* 0: announced local, 1: public (network order) */
    uint32_t ip; uint16_t port;                           /* the endpoint that answered */
    int heard, acknowledged, established;
    int gone; /* closed all its match sockets (T_BYE): reported dead, not probed again */
    int relayed, relay_ok; /* no direct path: traffic goes through the relay (registered there) */
    uint64_t added_ms;
} Peer;

typedef struct {
    int used, id, type, nonblocking; uint16_t vport, game_port;
    Packet *head, *tail; int count; /* received T_RAW datagrams */
} P2pSocket;

typedef ABI void (*RudpContextHandler)(int32_t ctx, int32_t event, int32_t error, void *arg);
typedef struct {
    int used, id, nonblocking, socket, state; /* state: 0 idle, 1 connecting, 2 established, 3 closed */
    uint16_t vport, remote_vport, member; RudpContextHandler handler; void *arg;
    uint32_t next_seq, expected; uint64_t started_ms, timeout_ms;
    Packet *unacked, *unacked_tail;     /* sent, waiting for their ACK */
    Packet *ready, *ready_tail; int ready_count; size_t ready_bytes; /* in order, for sceRudpRead */
    Packet *ahead;                      /* arrived early */
    int syn_heard;
} RudpContext;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t arrived = PTHREAD_COND_INITIALIZER;
static UdpSocket udp = INVALID_SOCKET;
static uint32_t local_ip;   /* network order */
static uint16_t local_port; /* network order */
static uint32_t public_ip;
static uint16_t public_port;
static uint16_t my_member;
static uint64_t room_id;
static Peer peers[MAX_PEERS];
static P2pSocket sockets[MAX_SOCKETS];
static RudpContext contexts[MAX_CONTEXTS];
static int32_t net_errno;
static int started, io_thread_started;

/* sceRudpSetEventHandler: Rudp's own events for the game, here SOCKET_RELEASED: once a terminated
 * context's socket has no Rudp context left, the game may close it (GODZILLA waits for this after a
 * match, 30 seconds at most). Delivered from the P2P thread, never from inside a Rudp call. */
typedef ABI int32_t (*RudpEventHandler)(int32_t event, int32_t socket, const uint8_t *data, uint64_t size,
                                         const void *address, uint32_t address_size, void *arg);
enum { RUDP_EVENT_SOCKET_RELEASED = 2 };
static RudpEventHandler rudp_handler;
static void *rudp_handler_arg;
static int32_t released[16];
static int released_count;

static void deliver_releases(void) {
    int32_t copy[16];
    pthread_mutex_lock(&lock);
    int n = released_count;
    memcpy(copy, released, sizeof(int32_t) * (size_t)n);
    released_count = 0;
    RudpEventHandler handler = rudp_handler;
    void *arg = rudp_handler_arg;
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < n; ++i) {
        if (runtime_online_debug()) printf("P2P: Rudp released socket %#x\n", copy[i]);
        if (handler) handler(RUDP_EVENT_SOCKET_RELEASED, copy[i], NULL, 0, NULL, 0, arg);
    }
}
static void (*established_cb)(uint16_t member, int up);

static uint64_t now_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
#endif
}

static const char *ip_text(uint32_t ip, char *buffer) {
    inet_ntop(AF_INET, &ip, buffer, 16);
    return buffer;
}

uint32_t p2p_virtual_address(uint16_t member) { return htonl(0x0A4D0000u | member); } /* 10.77.0.N */

static Peer *peer_by_member(uint16_t member) {
    for (int i = 0; i < MAX_PEERS; ++i) if (peers[i].used && peers[i].member == member) return &peers[i];
    return NULL;
}

static Peer *peer_by_virtual(uint32_t address) {
    uint32_t host = ntohl(address);
    if ((host & 0xFFFF0000u) != 0x0A4D0000u) return NULL;
    return peer_by_member((uint16_t)(host & 0xFFFF));
}

void p2p_set_established(void (*cb)(uint16_t member, int up)) { established_cb = cb; }

/* ---- the UDP socket --------------------------------------------------------------------------- */
static void send_to(uint32_t ip, uint16_t port, const void *data, size_t size) {
    if (udp == INVALID_SOCKET || !ip || !port) return;
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = ip;
    to.sin_port = port;
    sendto(udp, (const char *)data, (int)size, 0, (struct sockaddr *)&to, sizeof(to));
}

/* ---- the relay ----------------------------------------------------------------------------------
 * Players who cannot reach each other directly (strict NAT on both sides) send their packets through
 * the relay next to the shadNet server (online/server/relay.py): "MTRL", a type, a 16-byte token both
 * players derive from their room and member numbers, then the packet. A member that has not answered
 * direct probes after RELAY_AFTER_MS, or that reached us through the relay, is relayed from then on. */
enum { RELAY_AFTER_MS = 5000, RELAY_DEFAULT_PORT = 31316, RELAY_HEADER = 21,
       RELAY_REGISTER = 1, RELAY_REGISTERED = 2, RELAY_DATA = 3 };

static uint16_t relay_port(void) { /* network order */
    const char *env = getenv("MT_SHADNET_RELAY_PORT");
    int value = env ? atoi(env) : 0;
    return htons((uint16_t)(value > 0 && value < 65536 ? value : RELAY_DEFAULT_PORT));
}

static int force_relay(void) { /* MT_P2P_FORCE_RELAY=1: every member through the relay (tests) */
    const char *env = getenv("MT_P2P_FORCE_RELAY");
    return env && *env == '1';
}

static void relay_token(const Peer *p, uint8_t token[16]) {
    uint16_t low = my_member < p->member ? my_member : p->member, high = my_member < p->member ? p->member : my_member;
    uint64_t h[2] = {1469598103934665603ull, 1099511628211ull ^ (uint64_t)MT_ONLINE_PROTOCOL};
    const uint64_t parts[3] = {room_id, low, high};
    for (int k = 0; k < 2; ++k)
        for (int i = 0; i < 3; ++i)
            for (int b = 0; b < 8; ++b) h[k] = (h[k] ^ ((parts[i] >> (8 * b)) & 0xFF)) * (1099511628211ull + 2 * (uint64_t)k);
    memcpy(token, h, 16);
}

static void relay_register(const Peer *p) {
    uint8_t packet[RELAY_HEADER] = {'M', 'T', 'R', 'L', RELAY_REGISTER};
    relay_token(p, packet + 5);
    send_to(shadnet_server_ip(), relay_port(), packet, sizeof(packet));
}

static void send_packet(const Peer *p, uint8_t type, uint16_t src_vport, uint16_t dst_vport, uint32_t seq, uint32_t ack,
                        const void *payload, size_t size) {
    uint8_t buffer[RELAY_HEADER + sizeof(Header) + MAX_DATAGRAM];
    uint8_t *packet = buffer + RELAY_HEADER; /* room before it for the relay's header */
    if (size > MAX_DATAGRAM) return;
    Header h = {{'M', 'T'}, P2P_VERSION, type, my_member, src_vport, dst_vport, seq, ack, (uint16_t)size};
    memcpy(packet, &h, sizeof(h));
    if (size) memcpy(packet + sizeof(h), payload, size);
    if (p->relayed) {
        memcpy(buffer, "MTRL", 4);
        buffer[4] = RELAY_DATA;
        relay_token(p, buffer + 5);
        send_to(shadnet_server_ip(), relay_port(), buffer, RELAY_HEADER + sizeof(h) + size);
    } else if (p->ip) {
        send_to(p->ip, p->port, packet, sizeof(h) + size);
    } else if (type == T_PROBE) {
        for (int i = 0; i < 2; ++i) send_to(p->candidate_ip[i], p->candidate_port[i], packet, sizeof(h) + size);
    }
}

/* The STUN ping: the server answers with this socket's public address and keeps it for the others. */
static void stun_ping(void) {
    uint8_t packet[6 + 1 + 16 + 4] = {0xFF, 0x83, 0xFF, 0xFF, 0xFF, 0xFF, 0x01};
    const char *npid = shadnet_npid();
    memcpy(packet + 7, npid, strnlen(npid, 16));
    memcpy(packet + 23, &local_ip, 4);
    send_to(shadnet_server_ip(), htons(shadnet_signaling_port()), packet, sizeof(packet));
}

/* ---- Rudp internals (lock held) ---------------------------------------------------------------- */
static RudpContext *context_by_id(int id) {
    for (int i = 0; i < MAX_CONTEXTS; ++i) if (contexts[i].used && contexts[i].id == id) return &contexts[i];
    return NULL;
}

typedef struct { RudpContextHandler handler; int32_t ctx, event, error; void *arg; } PendingEvent;
static PendingEvent pending[64];
static int pending_count;
static uint16_t pending_up[MAX_PEERS]; /* members that just became reachable */
static int pending_up_count;
static uint16_t pending_down[MAX_PEERS]; /* members that just closed their match sockets */
static int pending_down_count;

static int debug_budget = 60; /* the first Rudp events and reads of a run are logged */

static void queue_event(RudpContext *c, int event, int32_t error) {
    if (runtime_online_debug() && debug_budget > 0) {
        --debug_budget;
        printf("P2P: Rudp context %d event %d (error %#x)%s\n", c->id, event, (unsigned)error,
               c->handler ? "" : " - the game has no handler");
    }
    if (!c->handler || pending_count >= 64) return;
    pending[pending_count++] = (PendingEvent){c->handler, c->id, event, error, c->arg};
}

/* Calls the game's handlers outside the lock (they may call back into Rudp). */
static void deliver_events(void) {
    PendingEvent copy[64];
    uint16_t up[MAX_PEERS], down[MAX_PEERS];
    pthread_mutex_lock(&lock);
    int n = pending_count, ups = pending_up_count, downs = pending_down_count;
    memcpy(down, pending_down, sizeof(uint16_t) * (size_t)downs);
    pending_down_count = 0;
    memcpy(copy, pending, sizeof(PendingEvent) * (size_t)n);
    memcpy(up, pending_up, sizeof(uint16_t) * (size_t)ups);
    pending_count = pending_up_count = 0;
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < ups; ++i) if (established_cb) established_cb(up[i], 1);
    for (int i = 0; i < downs; ++i) if (established_cb) established_cb(down[i], 0);
    for (int i = 0; i < n; ++i) copy[i].handler(copy[i].ctx, copy[i].event, copy[i].error, copy[i].arg);
}

static void free_list(Packet *p) {
    while (p) { Packet *next = p->next; free(p); p = next; }
}

static void context_established(RudpContext *c) {
    if (c->state == 2) return;
    c->state = 2;
    printf("P2P: Rudp context %d connected to member %u (vport %u -> %u)\n", c->id, c->member, c->vport, c->remote_vport);
    queue_event(c, RUDP_EVENT_ESTABLISHED, 0);
    queue_event(c, RUDP_EVENT_WRITABLE, 0);
    /* anything written before the connection was up goes out now */
    Peer *p = peer_by_member(c->member);
    for (Packet *q = c->unacked; q && p; q = q->next) {
        send_packet(p, T_DATA, c->vport, c->remote_vport, q->seq, 0, q->data, q->size);
        q->sent_ms = now_ms();
    }
}

static void context_failed(RudpContext *c, int32_t error) {
    if (c->state == 3) return;
    c->state = 3;
    printf("P2P: Rudp context %d to member %u failed (%#x)\n", c->id, c->member, (unsigned)error);
    queue_event(c, RUDP_EVENT_ERROR, error);
    queue_event(c, RUDP_EVENT_CLOSED, error);
    pthread_cond_broadcast(&arrived);
}

/* A packet for a Rudp context: the one bound to the vport for this member. */
static RudpContext *context_for(uint16_t vport, uint16_t member) {
    RudpContext *unattached = NULL;
    for (int i = 0; i < MAX_CONTEXTS; ++i) {
        RudpContext *c = &contexts[i];
        if (!c->used || c->vport != vport) continue;
        if (c->member == member) return c;
        if (!c->member && !unattached) unattached = c;
    }
    return unattached;
}

static void deliver_in_order(RudpContext *c) {
    for (;;) {
        Packet **link = &c->ahead, *found = NULL;
        while (*link && (*link)->seq != c->expected) link = &(*link)->next;
        if (!*link) break;
        found = *link;
        *link = found->next;
        found->next = NULL;
        if (c->ready_tail) c->ready_tail->next = found; else c->ready = found;
        c->ready_tail = found;
        c->ready_count++;
        c->ready_bytes += found->size;
        c->expected++;
        queue_event(c, RUDP_EVENT_READABLE, 0);
        pthread_cond_broadcast(&arrived);
    }
}

static void rudp_packet(Peer *p, const Header *h, const uint8_t *payload, size_t size) {
    RudpContext *c = context_for(h->dst_vport, p->member);
    if (runtime_online_debug() && h->type == T_DATA && debug_budget > 0) {
        --debug_budget;
        printf("P2P: Rudp data #%u from member %u to vport %u, %zu bytes%s:", h->seq, p->member, h->dst_vport, size,
               c ? "" : " (no context: dropped)");
        for (size_t i = 0; i < size && i < 24; ++i) printf(" %02x", payload[i]);
        printf("\n");
    }
    if (!c) return;
    switch (h->type) {
    case T_SYN:
        if (!c->member) { c->member = p->member; c->remote_vport = h->src_vport; }
        c->syn_heard = 1;
        send_packet(p, T_ACK, c->vport, c->remote_vport ? c->remote_vport : h->src_vport, 0, 0, NULL, 0); /* "SYN seen" */
        if (c->state == 1) context_established(c);
        break;
    case T_ACK: {
        if (h->seq == 0) { /* "SYN seen": connection setup only, never an acknowledgement of data */
            if (c->state == 1) context_established(c);
            break;
        }
        int cleared = 0;
        for (Packet **link = &c->unacked; *link;) {
            if ((*link)->seq <= h->ack) {
                Packet *done = *link;
                *link = done->next;
                free(done);
                cleared = 1;
            } else {
                link = &(*link)->next;
            }
        }
        c->unacked_tail = c->unacked;
        while (c->unacked_tail && c->unacked_tail->next) c->unacked_tail = c->unacked_tail->next;
        /* FLUSHED only when this acknowledgement delivered the last of what the game wrote */
        if (cleared && !c->unacked) queue_event(c, RUDP_EVENT_FLUSHED, 0);
        break;
    }
    case T_DATA: {
        if (c->state == 1 && c->syn_heard) context_established(c);
        if (h->seq >= c->expected) {
            int duplicate = 0;
            for (Packet *q = c->ahead; q; q = q->next) duplicate |= q->seq == h->seq;
            Packet *q = duplicate ? NULL : malloc(sizeof(Packet) + size);
            if (q) {
                memset(q, 0, sizeof(*q));
                q->seq = h->seq;
                q->size = (uint16_t)size;
                memcpy(q->data, payload, size);
                q->next = c->ahead;
                c->ahead = q;
                deliver_in_order(c);
            }
        }
        send_packet(p, T_ACK, c->vport, c->remote_vport, 1, c->expected - 1, NULL, 0);
        break;
    }
    case T_FIN:
        if (c->state != 3) {
            c->state = 3;
            printf("P2P: member %u closed Rudp context %d\n", p->member, c->id);
            queue_event(c, RUDP_EVENT_CLOSED, 0);
            pthread_cond_broadcast(&arrived);
        }
        break;
    }
}

/* ---- receiving -------------------------------------------------------------------------------- */
static void peer_heard(Peer *p, uint32_t ip, uint16_t port) {
    if (!p->ip || p->ip != ip || p->port != port) {
        char text[16];
        printf("P2P: member %u (%s) reachable at %s:%u\n", p->member, p->npid, ip_text(ip, text), ntohs(port));
    }
    p->ip = ip;
    p->port = port;
    p->heard = 1;
}

static void check_established(Peer *p) {
    if (p->established || p->gone || !p->heard || !p->acknowledged) return;
    p->established = 1;
    printf("P2P: connected to member %u (%s)\n", p->member, p->npid);
    if (pending_up_count < MAX_PEERS) pending_up[pending_up_count++] = p->member;
}

static void receive(const uint8_t *data, size_t size, uint32_t ip, uint16_t port) {
    if (ip == shadnet_server_ip() && port == htons(shadnet_signaling_port())) {
        if (size >= 12 && data[0] == 0xFF && data[1] == 0x83) {
            uint32_t mapped;
            uint16_t mapped_port;
            memcpy(&mapped, data + 6, 4);
            memcpy(&mapped_port, data + 10, 2);
            if (mapped != public_ip || mapped_port != public_port) {
                char text[16];
                printf("P2P: the server sees this player at %s:%u\n", ip_text(mapped, text), ntohs(mapped_port));
            }
            public_ip = mapped;
            public_port = mapped_port;
        }
        return;
    }
    int via_relay = 0;
    if (ip == shadnet_server_ip() && port == relay_port()) {
        if (size < RELAY_HEADER || memcmp(data, "MTRL", 4)) return;
        if (data[4] == RELAY_REGISTERED) {
            pthread_mutex_lock(&lock);
            for (int i = 0; i < MAX_PEERS; ++i) {
                uint8_t token[16];
                if (!peers[i].used) continue;
                relay_token(&peers[i], token);
                if (!memcmp(token, data + 5, 16) && !peers[i].relay_ok) {
                    peers[i].relay_ok = 1;
                    printf("P2P: the relay is ready for member %u\n", peers[i].member);
                }
            }
            pthread_mutex_unlock(&lock);
            return;
        }
        if (data[4] != RELAY_DATA) return;
        data += RELAY_HEADER;
        size -= RELAY_HEADER;
        via_relay = 1;
    }
    if (size < sizeof(Header)) return;
    Header h;
    memcpy(&h, data, sizeof(h));
    if (h.magic[0] != 'M' || h.magic[1] != 'T' || h.version != P2P_VERSION || sizeof(Header) + h.length > size) return;
    const uint8_t *payload = data + sizeof(Header);
    pthread_mutex_lock(&lock);
    Peer *p = peer_by_member(h.member);
    if (!p) { pthread_mutex_unlock(&lock); return; }
    if (via_relay) {
        if (!p->relayed) printf("P2P: member %u (%s) reached through the relay\n", p->member, p->npid);
        p->relayed = 1;
        p->heard = 1;
    } else {
        peer_heard(p, ip, port);
    }
    switch (h.type) {
    case T_PROBE:
        send_packet(p, T_PROBE_ACK, 0, 0, 0, 0, NULL, 0);
        break;
    case T_PROBE_ACK:
        p->acknowledged = 1;
        break;
    case T_BYE: /* its game closed its match sockets: on a PS4 the signaling connection goes dead */
        if (!p->gone) {
            p->gone = 1;
            if (p->established && pending_down_count < MAX_PEERS) pending_down[pending_down_count++] = p->member;
            p->established = 0;
            printf("P2P: member %u closed its match connection\n", p->member);
        }
        break;
    case T_RAW:
        for (int i = 0; i < MAX_SOCKETS; ++i) {
            P2pSocket *s = &sockets[i];
            if (!s->used || s->vport != h.dst_vport || s->count >= MAX_QUEUED) continue;
            Packet *q = malloc(sizeof(Packet) + h.length);
            if (!q) break;
            memset(q, 0, sizeof(*q));
            q->size = h.length;
            q->vport = h.src_vport;
            q->from = p2p_virtual_address(p->member);
            memcpy(q->data, payload, h.length);
            if (s->tail) s->tail->next = q; else s->head = q;
            s->tail = q;
            s->count++;
            pthread_cond_broadcast(&arrived);
            break;
        }
        break;
    default:
        rudp_packet(p, &h, payload, h.length);
        break;
    }
    check_established(p);
    pthread_mutex_unlock(&lock);
    deliver_events();
}

static void *io_main(void *unused) {
    (void)unused;
    runtime_thread_attach_host("P2P");
    uint8_t buffer[sizeof(Header) + MAX_DATAGRAM];
    uint64_t last_probe = 0, last_stun = 0, last_resend = 0;
    for (;;) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(udp, &read_set);
        struct timeval wait = {0, 20000};
        if (select((int)udp + 1, &read_set, NULL, NULL, &wait) > 0) {
            struct sockaddr_in from;
            socklen_t length = sizeof(from);
            int n = recvfrom(udp, (char *)buffer, sizeof(buffer), 0, (struct sockaddr *)&from, &length);
            if (n > 0) receive(buffer, (size_t)n, from.sin_addr.s_addr, from.sin_port);
        }
        uint64_t now = now_ms();
        if (now - last_stun >= STUN_MS) { stun_ping(); last_stun = now; }
        pthread_mutex_lock(&lock);
        if (now - last_probe >= PROBE_MS) {
            last_probe = now;
            for (int i = 0; i < MAX_PEERS; ++i) {
                Peer *q = &peers[i];
                if (!q->used || q->established || q->gone) continue;
                if (!q->relayed && (force_relay() || now - q->added_ms > RELAY_AFTER_MS)) {
                    q->relayed = 1;
                    printf("P2P: no direct path to member %u (%s): using the relay\n", q->member, q->npid);
                }
                if (q->relayed && !q->relay_ok) relay_register(q);
                send_packet(q, T_PROBE, 0, 0, 0, 0, NULL, 0);
            }
        }
        if (now - last_resend >= RESEND_MS) {
            last_resend = now;
            for (int i = 0; i < MAX_CONTEXTS; ++i) {
                RudpContext *c = &contexts[i];
                if (!c->used || !c->member) continue;
                Peer *p = peer_by_member(c->member);
                if (!p || !p->established) continue;
                if (c->state == 1) {
                    send_packet(p, T_SYN, c->vport, c->remote_vport, 0, 0, NULL, 0);
                    if (now - c->started_ms > (c->timeout_ms ? c->timeout_ms : CONNECT_TIMEOUT_MS))
                        context_failed(c, RUDP_ERROR(RUDP_CONN_TIMEOUT));
                } else if (c->state == 2) {
                    for (Packet *q = c->unacked; q; q = q->next) {
                        if (now - q->sent_ms < RESEND_MS) continue;
                        send_packet(p, T_DATA, c->vport, c->remote_vport, q->seq, 0, q->data, q->size);
                        q->sent_ms = now;
                    }
                }
            }
        }
        pthread_mutex_unlock(&lock);
        deliver_events();
        deliver_releases();
    }
    return NULL;
}

/* ---- the room (from runtime_np.c) ------------------------------------------------------------- */
static void local_address(void) {
    /* The address this PC uses towards the server (127.0.0.1 when the server runs here). */
    UdpSocket probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in to, me;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = shadnet_server_ip();
    to.sin_port = htons(shadnet_signaling_port());
    socklen_t length = sizeof(me);
    if (probe != INVALID_SOCKET && !connect(probe, (struct sockaddr *)&to, sizeof(to)) &&
        !getsockname(probe, (struct sockaddr *)&me, &length))
        local_ip = me.sin_addr.s_addr;
    if (probe != INVALID_SOCKET) close_socket(probe);
}

/* Opens the UDP socket and starts the I/O thread, once (after the shadNet sign-in). */
int p2p_start(void) {
    pthread_mutex_lock(&lock);
    if (started) { pthread_mutex_unlock(&lock); return 0; }
    UdpSocket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in at;
    memset(&at, 0, sizeof(at));
    at.sin_family = AF_INET;
    const char *env = getenv("MT_P2P_PORT");
    at.sin_port = htons((uint16_t)(env ? atoi(env) : 0));
    if (s == INVALID_SOCKET || bind(s, (struct sockaddr *)&at, sizeof(at))) {
        printf("P2P: cannot open UDP port %s\n", env ? env : "(any)");
        if (s != INVALID_SOCKET) close_socket(s);
        pthread_mutex_unlock(&lock);
        return -1;
    }
    socklen_t length = sizeof(at);
    getsockname(s, (struct sockaddr *)&at, &length);
    udp = s;
    local_port = at.sin_port;
    started = 1;
    local_address();
    char text[16];
    printf("P2P: UDP port %u open (local address %s)\n", ntohs(local_port), ip_text(local_ip, text));
    pthread_mutex_unlock(&lock);
    stun_ping();
    if (!io_thread_started) {
        io_thread_started = 1;
        pthread_t thread;
        pthread_create(&thread, NULL, io_main, NULL);
        pthread_detach(thread);
    }
    return 0;
}

/* Tells the room this player's local address (a shadNet room message the other copies read). */
static void *announce_main(void *unused) {
    (void)unused;
    runtime_thread_attach_host("P2PAnnounce");
    uint8_t message[5 + 4 + 2 + 2] = {'M', 'T', 'P', '2', 'P'};
    pthread_mutex_lock(&lock);
    memcpy(message + 5, &local_ip, 4);
    memcpy(message + 9, &local_port, 2);
    memcpy(message + 11, &my_member, 2);
    uint64_t room = room_id;
    pthread_mutex_unlock(&lock);
    if (!room) return NULL;
    PbOut out = {0};
    pb_uint(&out, 1, room);
    pb_uint(&out, 2, CAST_BROADCAST);
    pb_bytes(&out, 4, message, sizeof(message));
    uint8_t error = 0xFF;
    shadnet_request(ROOM_MESSAGE_COMMAND, &out, &error, NULL, NULL);
    pb_free(&out);
    return NULL;
}

/* From a thread of its own: callers may be on the shadNet reader thread, which must stay free to read
 * the reply. */
static void announce(void) {
    pthread_t thread;
    pthread_create(&thread, NULL, announce_main, NULL);
    pthread_detach(thread);
}

void p2p_room_joined(uint64_t room, uint16_t member) {
    pthread_mutex_lock(&lock);
    room_id = room;
    my_member = member;
    pthread_mutex_unlock(&lock);
    announce();
}

void p2p_member(uint16_t member, const char *npid, uint32_t public_address, uint16_t port) {
    if (member == my_member) return;
    pthread_mutex_lock(&lock);
    Peer *p = peer_by_member(member);
    for (int i = 0; i < MAX_PEERS && !p; ++i)
        if (!peers[i].used) { p = &peers[i]; memset(p, 0, sizeof(*p)); p->used = 1; p->member = member; p->added_ms = now_ms(); }
    if (p) {
        snprintf(p->npid, sizeof(p->npid), "%.16s", npid);
        if (public_address) { p->candidate_ip[1] = public_address; p->candidate_port[1] = port; }
    }
    pthread_mutex_unlock(&lock);
    announce(); /* the newcomer has not heard our local address yet */
}

void p2p_room_message(const uint8_t *message, size_t size, uint16_t from) {
    if (size < 13 || memcmp(message, "MTP2P", 5)) return;
    pthread_mutex_lock(&lock);
    Peer *p = peer_by_member(from);
    if (p) {
        memcpy(&p->candidate_ip[0], message + 5, 4);
        memcpy(&p->candidate_port[0], message + 9, 2);
        char text[16];
        if (runtime_online_debug()) printf("P2P: member %u announced %s:%u\n", from, ip_text(p->candidate_ip[0], text), ntohs(p->candidate_port[0]));
    }
    pthread_mutex_unlock(&lock);
}

void p2p_member_left(uint16_t member) {
    pthread_mutex_lock(&lock);
    Peer *p = peer_by_member(member);
    if (p) {
        for (int i = 0; i < MAX_CONTEXTS; ++i)
            if (contexts[i].used && contexts[i].member == member) context_failed(&contexts[i], RUDP_ERROR(RUDP_CONN_RESET));
        p->used = 0;
    }
    pthread_mutex_unlock(&lock);
    deliver_events();
}

void p2p_room_left(void) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_PEERS; ++i) peers[i].used = 0;
    room_id = 0;
    my_member = 0;
    pthread_mutex_unlock(&lock);
}

int p2p_member_established(uint16_t member) {
    pthread_mutex_lock(&lock);
    Peer *p = peer_by_member(member);
    int up = p && p->established;
    pthread_mutex_unlock(&lock);
    return up;
}

uint16_t p2p_game_port(void) {
    for (int i = 0; i < MAX_SOCKETS; ++i) if (sockets[i].used && sockets[i].game_port) return sockets[i].game_port;
    return htons(3658);
}

/* ---- sceNet: P2P sockets ------------------------------------------------------------------------ */
static int32_t net_fail(int e) { net_errno = e; return NET_ERROR(e); }

static P2pSocket *socket_by_id(int id) {
    for (int i = 0; i < MAX_SOCKETS; ++i) if (sockets[i].used && sockets[i].id == id) return &sockets[i];
    return NULL;
}

static ABI int32_t *net_errno_loc(void) { return &net_errno; }

static ABI int32_t net_socket(const char *name, int family, int type, int protocol) {
    (void)protocol;
    if (runtime_online_debug()) printf("P2P: sceNetSocket(%s, family %d, type %d)\n", name ? name : "", family, type);
    if (family != PS4_AF_INET || (type != PS4_SOCK_DGRAM_P2P && type != PS4_SOCK_STREAM_P2P))
        return net_fail(NET_ENETUNREACH); /* only P2P sockets reach anything */
    if (type == PS4_SOCK_STREAM_P2P) return net_fail(NET_EPROTONOSUPPORT);
    p2p_start();
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_SOCKETS; ++i) {
        if (sockets[i].used) continue;
        memset(&sockets[i], 0, sizeof(sockets[i]));
        sockets[i].used = 1;
        sockets[i].id = SOCKET_ID_BASE + i;
        sockets[i].type = type;
        pthread_mutex_unlock(&lock);
        return SOCKET_ID_BASE + i;
    }
    pthread_mutex_unlock(&lock);
    return net_fail(NET_EADDRINUSE);
}

static ABI int32_t net_bind(int id, const Ps4SockaddrIn *at, uint32_t length) {
    (void)length;
    pthread_mutex_lock(&lock);
    P2pSocket *s = socket_by_id(id);
    if (s && at) {
        s->vport = ntohs(at->vport); /* sockaddr fields are in network order */
        s->game_port = at->port;
        if (runtime_online_debug()) printf("P2P: socket %#x bound to port %u vport %u\n", id, ntohs(at->port), s->vport);
    }
    pthread_mutex_unlock(&lock);
    return s ? 0 : net_fail(NET_EBADF);
}

static ABI int32_t net_setsockopt(int id, int level, int name, const void *value, uint32_t length) {
    pthread_mutex_lock(&lock);
    P2pSocket *s = socket_by_id(id);
    if (s && level == PS4_SOL_SOCKET && name == PS4_SO_NBIO && value && length >= 4) s->nonblocking = *(const int32_t *)value != 0;
    pthread_mutex_unlock(&lock);
    return s ? 0 : net_fail(NET_EBADF);
}

static ABI int64_t net_sendto(int id, const void *data, uint64_t size, int flags, const Ps4SockaddrIn *to, uint32_t length) {
    (void)flags; (void)length;
    pthread_mutex_lock(&lock);
    P2pSocket *s = socket_by_id(id);
    Peer *p = s && to ? peer_by_virtual(to->addr) : NULL;
    if (s && p && size <= MAX_DATAGRAM) send_packet(p, T_RAW, s->vport, ntohs(to->vport), 0, 0, data, (size_t)size);
    static int logged;
    if (runtime_online_debug() && logged < 8 && s) {
        char text[16];
        printf("P2P: datagram from vport %u to %s vport %u, %llu bytes%s\n", s->vport, to ? ip_text(to->addr, text) : "?",
               to ? ntohs(to->vport) : 0, (unsigned long long)size, p ? "" : " (no such member: dropped)");
        ++logged;
    }
    pthread_mutex_unlock(&lock);
    if (!s) return net_fail(NET_EBADF);
    if (!p) return net_fail(NET_ENETUNREACH);
    return (int64_t)size;
}

static ABI int64_t net_recvfrom(int id, void *data, uint64_t size, int flags, Ps4SockaddrIn *from, uint32_t *length) {
    pthread_mutex_lock(&lock);
    P2pSocket *s = socket_by_id(id);
    while (s && !s->head && !s->nonblocking && !(flags & PS4_MSG_DONTWAIT)) {
        pthread_cond_wait(&arrived, &lock);
        s = socket_by_id(id);
    }
    if (!s) { pthread_mutex_unlock(&lock); return net_fail(NET_EBADF); }
    Packet *q = s->head;
    if (!q) { pthread_mutex_unlock(&lock); return net_fail(NET_EWOULDBLOCK); }
    s->head = q->next;
    if (!s->head) s->tail = NULL;
    s->count--;
    pthread_mutex_unlock(&lock);
    uint64_t n = q->size < size ? q->size : size;
    memcpy(data, q->data, (size_t)n);
    if (from) {
        memset(from, 0, sizeof(*from));
        from->len = sizeof(*from);
        from->family = PS4_AF_INET;
        from->addr = q->from;
        from->port = p2p_game_port();
        from->vport = htons(q->vport);
    }
    if (length) *length = sizeof(Ps4SockaddrIn);
    free(q);
    return (int64_t)n;
}

static ABI int32_t net_close(int id) {
    pthread_mutex_lock(&lock);
    P2pSocket *s = socket_by_id(id);
    if (s) { free_list(s->head); memset(s, 0, sizeof(*s)); }
    int open_sockets = 0;
    for (int i = 0; i < MAX_SOCKETS; ++i) open_sockets += sockets[i].used;
    if (s && !open_sockets) { /* the match is over here: the others hear it (a few times: UDP may drop one) */
        for (int i = 0; i < MAX_PEERS; ++i)
            if (peers[i].used && peers[i].established)
                for (int k = 0; k < 3; ++k) send_packet(&peers[i], T_BYE, 0, 0, 0, 0, NULL, 0);
    }
    pthread_cond_broadcast(&arrived);
    pthread_mutex_unlock(&lock);
    deliver_events();
    return s ? 0 : net_fail(NET_EBADF);
}

static ABI int32_t net_get_sock_info(int id, void *info, int count, int flags) {
    (void)info; (void)count; (void)flags;
    printf("P2P: sceNetGetSockInfo(%#x) not answered\n", id);
    return net_fail(NET_EINVAL);
}

/* ---- Rudp ---------------------------------------------------------------------------------------- */
static ABI int32_t rudp_ok(void) { return 0; }

static ABI int32_t rudp_set_event_handler(RudpEventHandler handler, void *arg) {
    pthread_mutex_lock(&lock);
    rudp_handler = handler;
    rudp_handler_arg = arg;
    pthread_mutex_unlock(&lock);
    return 0;
}

static ABI int32_t rudp_enable_io_thread(uint32_t stack, uint32_t priority) {
    (void)stack; (void)priority;
    return 0; /* the P2P thread does Rudp's I/O */
}

static ABI int32_t rudp_create_context(RudpContextHandler handler, void *arg, int32_t *id) {
    if (!id) return RUDP_ERROR(RUDP_INVALID_ARGUMENT);
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_CONTEXTS; ++i) {
        if (contexts[i].used) continue;
        memset(&contexts[i], 0, sizeof(contexts[i]));
        contexts[i].used = 1;
        contexts[i].id = i + 1;
        contexts[i].handler = handler;
        contexts[i].arg = arg;
        contexts[i].next_seq = 1;
        contexts[i].expected = 1;
        *id = i + 1;
        pthread_mutex_unlock(&lock);
        if (runtime_online_debug()) printf("P2P: Rudp context %d created\n", i + 1);
        return 0;
    }
    pthread_mutex_unlock(&lock);
    return RUDP_ERROR(RUDP_TOO_MANY_CONTEXTS);
}

static ABI int32_t rudp_bind(int32_t id, int32_t socket, uint16_t vport, uint8_t mux) {
    pthread_mutex_lock(&lock);
    RudpContext *c = context_by_id(id);
    if (c) { c->socket = socket; c->vport = vport; }
    pthread_mutex_unlock(&lock);
    if (runtime_online_debug()) printf("P2P: Rudp context %d bound to socket %#x vport %u (mux %u)\n", id, socket, vport, mux);
    return c ? 0 : RUDP_ERROR(RUDP_INVALID_CONTEXT);
}

static ABI int32_t rudp_initiate(int32_t id, const Ps4SockaddrIn *to, uint32_t length, int32_t flags) {
    (void)length; (void)flags;
    if (!to) return RUDP_ERROR(RUDP_INVALID_ARGUMENT);
    pthread_mutex_lock(&lock);
    RudpContext *c = context_by_id(id);
    Peer *p = peer_by_virtual(to->addr);
    char text[16];
    printf("P2P: Rudp context %d connects to %s vport %u (member %u)\n", id, ip_text(to->addr, text), ntohs(to->vport),
           p ? p->member : 0);
    int32_t result = 0;
    if (!c) result = RUDP_ERROR(RUDP_INVALID_CONTEXT);
    else if (!c->vport) result = RUDP_ERROR(RUDP_NOT_BOUND);
    else if (!p) result = RUDP_ERROR(RUDP_INVALID_ARGUMENT);
    else {
        c->member = p->member;
        c->remote_vport = ntohs(to->vport);
        c->state = 1;
        c->started_ms = now_ms();
        if (p->established) send_packet(p, T_SYN, c->vport, c->remote_vport, 0, 0, NULL, 0);
        if (c->syn_heard) context_established(c);
    }
    pthread_mutex_unlock(&lock);
    deliver_events();
    return result;
}

static ABI int32_t rudp_write(int32_t id, const void *data, uint64_t size, uint8_t flags) {
    (void)flags;
    if (!data || size > MAX_DATAGRAM) return RUDP_ERROR(RUDP_INVALID_ARGUMENT);
    pthread_mutex_lock(&lock);
    RudpContext *c = context_by_id(id);
    if (!c) { pthread_mutex_unlock(&lock); return RUDP_ERROR(RUDP_INVALID_CONTEXT); }
    if (c->state == 3) { pthread_mutex_unlock(&lock); return RUDP_ERROR(RUDP_CONN_RESET); }
    Packet *q = malloc(sizeof(Packet) + size);
    if (!q) { pthread_mutex_unlock(&lock); return RUDP_ERROR(RUDP_INVALID_ARGUMENT); }
    memset(q, 0, sizeof(*q));
    q->seq = c->next_seq++;
    q->size = (uint16_t)size;
    memcpy(q->data, data, (size_t)size);
    if (c->unacked_tail) c->unacked_tail->next = q; else c->unacked = q;
    c->unacked_tail = q;
    Peer *p = peer_by_member(c->member);
    if (c->state == 2 && p) {
        send_packet(p, T_DATA, c->vport, c->remote_vport, q->seq, 0, q->data, q->size);
        q->sent_ms = now_ms();
    }
    pthread_mutex_unlock(&lock);
    return (int32_t)size;
}

static ABI int32_t rudp_read(int32_t id, void *data, uint64_t size, uint8_t flags, void *info) {
    pthread_mutex_lock(&lock);
    if (runtime_online_debug() && debug_budget > 0) {
        --debug_budget;
        RudpContext *d = context_by_id(id);
        printf("P2P: sceRudpRead(context %d, %llu bytes, flags %#x, info %s): %d message(s) waiting\n", id,
               (unsigned long long)size, flags, info ? "given" : "none", d ? d->ready_count : -1);
    }
    RudpContext *c = context_by_id(id);
    while (c && !c->ready && c->state != 3 && !c->nonblocking && !(flags & RUDP_MSG_DONTWAIT)) {
        pthread_cond_wait(&arrived, &lock);
        c = context_by_id(id);
    }
    if (!c) { pthread_mutex_unlock(&lock); return RUDP_ERROR(RUDP_INVALID_CONTEXT); }
    Packet *q = c->ready;
    if (!q) {
        int32_t result = c->state == 3 ? RUDP_ERROR(RUDP_CONN_RESET) : RUDP_ERROR(RUDP_WOULDBLOCK);
        pthread_mutex_unlock(&lock);
        return result;
    }
    c->ready = q->next;
    if (!c->ready) c->ready_tail = NULL;
    c->ready_count--;
    c->ready_bytes -= q->size;
    pthread_mutex_unlock(&lock);
    uint64_t n = q->size < size ? q->size : size;
    if (data) memcpy(data, q->data, (size_t)n);
    free(q);
    return (int32_t)n;
}

static ABI int32_t rudp_size_readable(int32_t id) {
    pthread_mutex_lock(&lock);
    RudpContext *c = context_by_id(id);
    int32_t n = c ? (int32_t)c->ready_bytes : RUDP_ERROR(RUDP_INVALID_CONTEXT);
    static int logged;
    if (runtime_online_debug() && n > 0 && logged < 5) { ++logged; printf("P2P: sceRudpGetSizeReadable(%d) = %d\n", id, n); }
    pthread_mutex_unlock(&lock);
    return n;
}

static ABI int32_t rudp_packets_to_read(int32_t id) {
    pthread_mutex_lock(&lock);
    RudpContext *c = context_by_id(id);
    int32_t n = c ? c->ready_count : RUDP_ERROR(RUDP_INVALID_CONTEXT);
    static int logged;
    if (runtime_online_debug() && n > 0 && logged < 5) { ++logged; printf("P2P: sceRudpGetNumberOfPacketsToRead(%d) = %d\n", id, n); }
    pthread_mutex_unlock(&lock);
    return n;
}

static ABI int32_t rudp_set_option(int32_t id, int32_t option, const void *value, uint64_t length) {
    pthread_mutex_lock(&lock);
    RudpContext *c = context_by_id(id);
    if (c && option == RUDP_OPTION_NONBLOCK && value && length >= 4) c->nonblocking = *(const int32_t *)value != 0;
    if (c && option == RUDP_OPTION_CONNECTION_TIMEOUT && value && length >= 4) c->timeout_ms = *(const uint32_t *)value;
    pthread_mutex_unlock(&lock);
    if (runtime_online_debug()) printf("P2P: Rudp context %d option %d = %d\n", id, option, value && length >= 4 ? *(const int32_t *)value : -1);
    return 0;
}

static ABI int32_t rudp_flush(int32_t id) {
    (void)id;
    return 0; /* writes go out at once; resends are the P2P thread's */
}

static ABI int32_t rudp_terminate(int32_t id) {
    pthread_mutex_lock(&lock);
    RudpContext *c = context_by_id(id);
    if (c) {
        Peer *p = peer_by_member(c->member);
        if (p && c->state == 2) send_packet(p, T_FIN, c->vport, c->remote_vport, 0, 0, NULL, 0);
        int32_t socket = c->socket;
        free_list(c->unacked);
        free_list(c->ready);
        free_list(c->ahead);
        memset(c, 0, sizeof(*c));
        int still_used = 0;
        for (int i = 0; i < MAX_CONTEXTS; ++i) still_used |= contexts[i].used && contexts[i].socket == socket;
        if (socket && !still_used && released_count < 16) released[released_count++] = socket;
        pthread_cond_broadcast(&arrived);
    }
    pthread_mutex_unlock(&lock);
    printf("P2P: Rudp context %d terminated\n", id);
    return c ? 0 : RUDP_ERROR(RUDP_INVALID_CONTEXT);
}

static const RuntimeExport exports[] = {
    {"sceNetErrnoLoc", net_errno_loc},
    {"sceNetSocket", net_socket},
    {"sceNetBind", net_bind},
    {"sceNetSetsockopt", net_setsockopt},
    {"sceNetSendto", net_sendto},
    {"sceNetRecvfrom", net_recvfrom},
    {"sceNetSocketClose", net_close},
    {"sceNetGetSockInfo", net_get_sock_info},
    {"sceRudpInit", rudp_ok},
    {"sceRudpEnd", rudp_ok},
    {"sceRudpSetEventHandler", rudp_set_event_handler},
    {"sceRudpEnableInternalIOThread", rudp_enable_io_thread},
    {"sceRudpCreateContext", rudp_create_context},
    {"sceRudpBind", rudp_bind},
    {"sceRudpInitiate", rudp_initiate},
    {"sceRudpWrite", rudp_write},
    {"sceRudpRead", rudp_read},
    {"sceRudpGetSizeReadable", rudp_size_readable},
    {"sceRudpGetNumberOfPacketsToRead", rudp_packets_to_read},
    {"sceRudpSetOption", rudp_set_option},
    {"sceRudpFlush", rudp_flush},
    {"sceRudpNetFlush", rudp_ok},
    {"sceRudpTerminate", rudp_terminate},
};

uintptr_t runtime_p2p_resolve(const char *name) { return shadnet_enabled() ? RUNTIME_LOOKUP(exports, name) : 0; }
