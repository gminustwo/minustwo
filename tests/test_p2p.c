/* P2P (src/runtime_p2p.c) against a scripted peer on loopback: a reached member keeps getting keep-alive probes
 * and is reported dead once it goes silent, its Rudp contexts failing; Rudp keeps only RUDP_WINDOW messages ahead
 * of the next expected one, and at most RUDP_MAX_READY the game has not read. */
#undef NDEBUG /* the checks are asserts; release builds define NDEBUG */
#define P2P_KEEPALIVE_MS 100
#define P2P_PEER_TIMEOUT_MS 800
#include "../src/runtime_p2p.c"
#include <assert.h>

/* What runtime_p2p.c uses from the rest of the runtime: no shadNet server here (STUN and the relay are not
 * reached; local addresses come from p2p_room_message as the server would carry them). */
void runtime_thread_attach_host(const char *name) { (void)name; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    (void)table; (void)count; (void)nid;
    return 0;
}
int runtime_online_debug(void) { return 0; }
int shadnet_enabled(void) { return 1; }
const char *shadnet_npid(void) { return "tester"; }
uint32_t shadnet_server_ip(void) { return 0; }
uint16_t shadnet_signaling_port(void) { return 31314; }
int shadnet_request(uint16_t command, const PbOut *request, uint8_t *error, uint8_t **reply, uint32_t *reply_size) {
    (void)command; (void)request; (void)error; (void)reply; (void)reply_size;
    return -1;
}
void pb_uint(PbOut *out, unsigned field, uint64_t value) { (void)out; (void)field; (void)value; }
void pb_bytes(PbOut *out, unsigned field, const void *data, size_t size) { (void)out; (void)field; (void)data; (void)size; }
void pb_free(PbOut *out) { (void)out; }

static void sleep_ms(unsigned ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
#endif
}

static int load(const int *value) { return __atomic_load_n(value, __ATOMIC_SEQ_CST); }
static int wait_for(const int *value, int expected, unsigned ms) {
    for (unsigned waited = 0; load(value) != expected && waited < ms; waited += 10) sleep_ms(10);
    return load(value) == expected;
}

/* ---- what the game would hear ---- */
static int ups[8], downs[8];
static void on_established(uint16_t member, int up) { __atomic_add_fetch(up ? &ups[member] : &downs[member], 1, __ATOMIC_SEQ_CST); }

typedef struct { int count[8]; int32_t error[8]; } ContextEvents; /* by Rudp event */
static pthread_mutex_t events_lock = PTHREAD_MUTEX_INITIALIZER;
static ABI void on_rudp(int32_t ctx, int32_t event, int32_t error, void *arg) {
    (void)ctx;
    ContextEvents *e = arg;
    pthread_mutex_lock(&events_lock);
    if (event >= 0 && event < 8) { e->count[event]++; e->error[event] = error; }
    pthread_mutex_unlock(&events_lock);
}
static int wait_event(ContextEvents *e, int event, unsigned ms) {
    for (unsigned waited = 0;; waited += 10) {
        pthread_mutex_lock(&events_lock);
        int n = e->count[event];
        pthread_mutex_unlock(&events_lock);
        if (n || waited >= ms) return n;
        sleep_ms(10);
    }
}

/* ---- the other player: one UDP socket, answering probes and SYNs as member `answer_as` (0: silent) ---- */
static UdpSocket fake = INVALID_SOCKET;
static uint16_t fake_port; /* network order */
static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fake_heard = PTHREAD_COND_INITIALIZER;
static int answer_as, probes, data_acks;

static void fake_send(uint16_t member, uint8_t type, uint16_t vport, uint32_t seq, const void *payload, uint16_t size) {
    uint8_t packet[sizeof(Header) + 16];
    Header h = {{'M', 'T'}, P2P_VERSION, type, member, vport, vport, seq, 0, size};
    memcpy(packet, &h, sizeof(h));
    if (size) memcpy(packet + sizeof(h), payload, size);
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    to.sin_port = local_port;
    sendto(fake, (const char *)packet, (int)(sizeof(h) + size), 0, (struct sockaddr *)&to, sizeof(to));
}

static void *fake_main(void *unused) {
    (void)unused;
    uint8_t buffer[sizeof(Header) + MAX_DATAGRAM];
    for (;;) {
        int n = recvfrom(fake, (char *)buffer, sizeof(buffer), 0, NULL, NULL);
        if (n < (int)sizeof(Header)) continue;
        Header h;
        memcpy(&h, buffer, sizeof(h));
        pthread_mutex_lock(&fake_lock);
        int as = answer_as;
        if (h.type == T_PROBE) ++probes;
        if (h.type == T_ACK && h.seq == 1) ++data_acks; /* seq 0 is "SYN seen" */
        pthread_cond_broadcast(&fake_heard);
        pthread_mutex_unlock(&fake_lock);
        if (as && h.type == T_PROBE) fake_send((uint16_t)as, T_PROBE_ACK, 0, 0, NULL, 0);
        if (as && h.type == T_SYN) fake_send((uint16_t)as, T_ACK, h.dst_vport, 0, NULL, 0);
    }
    return NULL;
}

static int fake_get(const int *value) {
    pthread_mutex_lock(&fake_lock);
    int n = *value;
    pthread_mutex_unlock(&fake_lock);
    return n;
}
static void fake_answer_as(int member) {
    pthread_mutex_lock(&fake_lock);
    answer_as = member;
    pthread_mutex_unlock(&fake_lock);
}

/* Rudp data #seq (its own number as payload) from `member` on `vport`, once its acknowledgement is back. */
static void send_data(uint16_t member, uint16_t vport, uint32_t seq) {
    pthread_mutex_lock(&fake_lock);
    int before = data_acks;
    pthread_mutex_unlock(&fake_lock);
    fake_send(member, T_DATA, vport, seq, &seq, sizeof(seq));
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 2;
    pthread_mutex_lock(&fake_lock);
    while (data_acks == before)
        if (pthread_cond_timedwait(&fake_heard, &fake_lock, &until)) break;
    int acked = data_acks != before;
    pthread_mutex_unlock(&fake_lock);
    assert(acked);
}

/* The member's announcement of its local address (a shadNet room message), here the fake's socket. */
static void announce_fake(uint16_t member) {
    uint8_t message[13] = {'M', 'T', 'P', '2', 'P'};
    uint32_t ip = htonl(INADDR_LOOPBACK);
    memcpy(message + 5, &ip, 4);
    memcpy(message + 9, &fake_port, 2);
    memcpy(message + 11, &member, 2);
    p2p_room_message(message, sizeof(message), member);
}

static RudpContext *connect_context(ContextEvents *events, uint16_t member, uint16_t vport) {
    int32_t id = 0;
    assert(rudp_create_context(on_rudp, events, &id) == 0);
    assert(rudp_bind(id, SOCKET_ID_BASE, vport, 0) == 0);
    Ps4SockaddrIn to;
    memset(&to, 0, sizeof(to));
    to.len = sizeof(to);
    to.family = PS4_AF_INET;
    to.addr = p2p_virtual_address(member);
    to.vport = htons(vport);
    assert(rudp_initiate(id, &to, sizeof(to), 0) == 0);
    assert(wait_event(events, RUDP_EVENT_ESTABLISHED, 3000));
    return context_by_id(id);
}

static void context_state(RudpContext *c, int *ahead, int *ready, uint32_t *expected) {
    pthread_mutex_lock(&lock);
    *ahead = 0;
    for (Packet *q = c->ahead; q; q = q->next) ++*ahead;
    *ready = c->ready_count;
    *expected = c->expected;
    pthread_mutex_unlock(&lock);
}

static uint32_t read_one(RudpContext *c) {
    uint32_t seq = 0;
    assert(rudp_read(c->id, &seq, sizeof(seq), RUDP_MSG_DONTWAIT, NULL) == (int32_t)sizeof(seq));
    return seq;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    p2p_set_established(on_established);
    assert(p2p_start() == 0);
    fake = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in at;
    memset(&at, 0, sizeof(at));
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(fake != INVALID_SOCKET && !bind(fake, (struct sockaddr *)&at, sizeof(at)));
    socklen_t length = sizeof(at);
    assert(!getsockname(fake, (struct sockaddr *)&at, &length));
    fake_port = at.sin_port;
    pthread_t thread;
    pthread_create(&thread, NULL, fake_main, NULL);
    pthread_detach(thread);

    /* member 2 is reached through the address it announced */
    fake_answer_as(2);
    p2p_room_joined(0x1234, 1);
    p2p_member(2, "alice", 0, 0);
    announce_fake(2);
    assert(wait_for(&ups[2], 1, 3000));
    ContextEvents events_a;
    memset(&events_a, 0, sizeof(events_a));
    connect_context(&events_a, 2, 5);

    /* while it answers, it keeps getting probes (and so do the routers between) and stays up */
    int before = fake_get(&probes);
    sleep_ms(2 * P2P_PEER_TIMEOUT_MS);
    assert(fake_get(&probes) - before >= 4);
    assert(load(&downs[2]) == 0 && p2p_member_established(2));

    /* it goes silent: dead after P2P_PEER_TIMEOUT_MS, not before; its Rudp context fails */
    fake_answer_as(0);
    uint64_t silent = now_ms();
    assert(wait_for(&downs[2], 1, P2P_PEER_TIMEOUT_MS + 3000));
    uint64_t took = now_ms() - silent;
    printf("p2p-test: silent member reported dead after %llu ms\n", (unsigned long long)took);
    assert(took + PROBE_MS + 100 >= P2P_PEER_TIMEOUT_MS);
    assert(!p2p_member_established(2));
    assert(wait_event(&events_a, RUDP_EVENT_CLOSED, 1000));
    pthread_mutex_lock(&events_lock);
    assert(events_a.count[RUDP_EVENT_ERROR] == 1 && events_a.error[RUDP_EVENT_ERROR] == RUDP_ERROR(RUDP_CONN_TIMEOUT));
    pthread_mutex_unlock(&events_lock);
    /* and it is left alone: no more probes, and a late packet from it does not bring it back */
    sleep_ms(2 * PROBE_MS);
    before = fake_get(&probes);
    sleep_ms(4 * PROBE_MS);
    assert(fake_get(&probes) == before);
    fake_send(2, T_PROBE, 0, 0, NULL, 0);
    sleep_ms(2 * PROBE_MS);
    assert(load(&ups[2]) == 1 && load(&downs[2]) == 1 && !p2p_member_established(2));

    /* member 3, and a Rudp context to it */
    fake_answer_as(3);
    p2p_member(3, "bob", 0, 0);
    announce_fake(3);
    assert(wait_for(&ups[3], 1, 3000));
    ContextEvents events_b;
    memset(&events_b, 0, sizeof(events_b));
    RudpContext *c = connect_context(&events_b, 3, 6);
    int ahead, ready;
    uint32_t expected;
    context_state(c, &ahead, &ready, &expected);
    assert(ahead == 0 && ready == 0 && expected == 1);

    /* beyond the window: acknowledged as nothing new, not kept */
    send_data(3, 6, 1 + RUDP_WINDOW);
    context_state(c, &ahead, &ready, &expected);
    assert(ahead == 0 && ready == 0 && expected == 1);
    /* inside it, with #1 missing: kept */
    for (uint32_t seq = 2; seq <= RUDP_WINDOW; ++seq) send_data(3, 6, seq);
    context_state(c, &ahead, &ready, &expected);
    assert(ahead == RUDP_WINDOW - 1 && ready == 0);
    /* a flood of later ones: nothing more is kept */
    for (uint32_t i = 0; i < 1000; ++i) send_data(3, 6, 1 + RUDP_WINDOW + i);
    context_state(c, &ahead, &ready, &expected);
    assert(ahead == RUDP_WINDOW - 1 && ready == 0 && expected == 1);
    /* #1 arrives: everything kept is delivered in order */
    send_data(3, 6, 1);
    context_state(c, &ahead, &ready, &expected);
    assert(ahead == 0 && ready == RUDP_WINDOW && expected == RUDP_WINDOW + 1);
    assert(read_one(c) == 1 && read_one(c) == 2);

    /* the game stops reading: RUDP_MAX_READY messages wait, later ones are left to the sender's resend */
    for (uint32_t seq = RUDP_WINDOW + 1; seq <= RUDP_WINDOW + RUDP_MAX_READY; ++seq) send_data(3, 6, seq);
    context_state(c, &ahead, &ready, &expected);
    uint32_t taken = RUDP_MAX_READY - (RUDP_WINDOW - 2);
    assert(ahead == 0 && ready == RUDP_MAX_READY && expected == RUDP_WINDOW + 1 + taken);
    /* it reads one: the next one is taken again, in order */
    assert(read_one(c) == 3);
    send_data(3, 6, expected);
    context_state(c, &ahead, &ready, &expected);
    assert(ready == RUDP_MAX_READY && expected == RUDP_WINDOW + 2 + taken);
    assert(read_one(c) == 4);

    /* member 3 stayed up all along: its probes were answered */
    assert(load(&downs[3]) == 0 && p2p_member_established(3));

    /* a new room, without leaving this one (GODZILLA searches again when the host leaves as the match
     * loads): member ids repeat, so member 2, gone in the last room, is a newcomer here and is reached */
    fake_answer_as(2);
    p2p_room_joined(0x5678, 1);
    p2p_member(2, "carol", 0, 0);
    announce_fake(2);
    assert(wait_for(&ups[2], 2, 3000));
    assert(p2p_member_established(2));
    /* and the last room's members are not carried over */
    assert(!p2p_member_established(3));
    puts("p2p-test: ok");
    return 0;
}
