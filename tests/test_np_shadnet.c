/* runtime_np.c's shadNet matchmaking when the server session ends and comes back: the game hears its room
 * destroyed by a connection error and the other members' signaling dead, P2P lets those members go, and after
 * the next sign-in the matching contexts the game still has are started again on the server, once. */
#undef NDEBUG /* the checks are asserts; release builds define NDEBUG */
#include "../src/runtime_np.c"
#include <assert.h>

/* ---- the rest of the runtime ---- */
void runtime_thread_attach_host(const char *name) { (void)name; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    (void)table; (void)count; (void)nid;
    return 0;
}
int runtime_online_debug(void) { return 0; }

/* shadNet: requests are recorded (a context start: its context id), and answered without error */
int shadnet_enabled(void) { return 1; }
const char *shadnet_npid(void) { return "tester"; }
void shadnet_set_notify(ShadnetNotify notify) { (void)notify; }
void shadnet_set_session(ShadnetSession session) { (void)session; }
static int context_starts;
static uint64_t started_context;
/* pb_uint keeps (field, value) pairs, enough to read a request back */
void pb_uint(PbOut *out, unsigned field, uint64_t value) {
    uint64_t pair[2] = {field, value};
    uint8_t *grown = realloc(out->data, out->size + sizeof(pair));
    if (!grown) { out->failed = 1; return; }
    out->data = grown;
    memcpy(out->data + out->size, pair, sizeof(pair));
    out->size += sizeof(pair);
}
void pb_bool(PbOut *out, unsigned field, int value) { (void)out; (void)field; (void)value; }
void pb_bytes(PbOut *out, unsigned field, const void *data, size_t size) { (void)out; (void)field; (void)data; (void)size; }
void pb_msg(PbOut *out, unsigned field, const PbOut *inner) { (void)out; (void)field; (void)inner; }
int pb_next(PbIn *in, unsigned *field, uint64_t *value, const uint8_t **data, size_t *size) {
    (void)in; (void)field; (void)value; (void)data; (void)size;
    return 0;
}
void pb_free(PbOut *out) {
    free(out->data);
    memset(out, 0, sizeof(*out));
}
int shadnet_request(uint16_t command, const PbOut *request, uint8_t *error, uint8_t **reply, uint32_t *reply_size) {
    if (command == SN_CONTEXT_START) {
        uint64_t pair[2] = {0, 0};
        if (request && request->size >= sizeof(pair)) memcpy(pair, request->data, sizeof(pair));
        assert(pair[0] == 1);
        started_context = pair[1];
        ++context_starts;
    }
    if (error) *error = 0;
    if (reply) *reply = NULL;
    if (reply_size) *reply_size = 0;
    return 0;
}

/* P2P: the members let go are recorded */
static uint16_t members_left[8];
static int members_left_count, rooms_left;
void p2p_member_left(uint16_t member) { members_left[members_left_count++] = member; }
void p2p_room_left(void) { ++rooms_left; }
int p2p_start(void) { return 0; }
void p2p_set_established(void (*cb)(uint16_t member, int up)) { (void)cb; }
void p2p_room_joined(uint64_t room_id, uint16_t member) { (void)room_id; (void)member; }
void p2p_member(uint16_t member, const char *npid, uint32_t address, uint16_t port) {
    (void)member; (void)npid; (void)address; (void)port;
}
void p2p_room_message(const uint8_t *message, size_t size, uint16_t from) { (void)message; (void)size; (void)from; }
int p2p_member_established(uint16_t member) { (void)member; return 0; }
uint32_t p2p_virtual_address(uint16_t member) { (void)member; return 0; }
uint16_t p2p_game_port(void) { return 0; }

/* The game's callbacks: never called (no event thread here); the queued events are read instead. */
static ABI void room_callback(uint16_t ctx, uint64_t room_id, uint16_t event, const void *data, void *arg) {
    (void)ctx; (void)room_id; (void)event; (void)data; (void)arg;
}
static ABI void signaling_callback(uint16_t ctx, uint64_t room_id, uint16_t member, uint16_t event, int32_t error, void *arg) {
    (void)ctx; (void)room_id; (void)member; (void)event; (void)error; (void)arg;
}

static Event *next_event(void) {
    Event *e = queue_head;
    if (e) {
        queue_head = e->next;
        if (!queue_head) queue_tail = NULL;
    }
    return e;
}

/* FNV-1a as match_key() computes it, of the text a copy's key is made from */
static uint32_t key_of(const char *text) {
    uint32_t h = 2166136261u;
    for (const char *p = text; *p; ++p) h = (h ^ (uint8_t)*p) * 16777619u;
    return (h & 0x7FFFFFFF) | 1;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    /* the match key: the US release keeps the key of its version alone (copies of earlier builds still meet),
     * the European release has its title ID before the version, so its copies meet only each other */
    char text[128];
    runtime_np_set_game("CUSA02046", "01.01");
    assert(!strcmp(game_title, "") && !strcmp(game_version, "01.01"));
    runtime_np_set_game("", "01.00");
    assert(!strcmp(game_title, ""));
    runtime_np_set_game("CUSA02058", "01.00");
    runtime_np_set_exact_math(1);
    snprintf(text, sizeof(text), "minustwo-online/%d/CUSA02058-01.00/", MT_ONLINE_PROTOCOL);
    assert(match_key() == key_of(text));
    /* a started context, in a room with member 2 (this player is member 1) */
    contexts[0].used = 1;
    contexts[0].started = 1;
    contexts[0].id = 7;
    contexts[0].room_cb = room_callback;
    contexts[0].signaling_cb = signaling_callback;
    room_context = 7;
    room.active = 1;
    room.room_id = 0x99;
    room.members[0].used = 1;
    room.members[0].member_id = 1;
    room.members[1].used = 1;
    room.members[1].member_id = 2;
    my_member = 1;
    assert(runtime_online_in_room());

    /* the first sign-in: nothing to start again */
    shadnet_session(1);
    assert(context_starts == 0);

    /* the session ends */
    shadnet_session(0);
    Event *e = next_event();
    assert(e && e->kind == EVENT_ROOM && e->event == M2_ROOM_EVENT_ROOM_DESTROYED && e->room == 0x99 && e->ctx == 7);
    assert(e->data && ((M2RoomUpdate *)e->data)->cause == M2_CAUSE_CONNECTION_ERROR);
    free(e->data);
    free(e);
    e = next_event();
    assert(e && e->kind == EVENT_SIGNALING && e->event == M2_SIGNALING_EVENT_DEAD && e->member == 2 && e->room == 0x99);
    free(e);
    assert(!next_event());
    assert(!runtime_online_in_room() && my_member == 0); /* the fight cheats may come back */
    assert(members_left_count == 1 && members_left[0] == 2 && rooms_left == 1);

    /* signed in again: the context the game still has is started again, once */
    shadnet_session(1);
    assert(context_starts == 1 && started_context == 7);
    shadnet_session(1);
    assert(context_starts == 1);

    /* a session that ends outside a room: no room events, the context is started again after the sign-in */
    shadnet_session(0);
    assert(!next_event() && members_left_count == 1 && rooms_left == 1);
    shadnet_session(1);
    assert(context_starts == 2 && started_context == 7);

    /* a context the game stopped is not started again */
    contexts[0].started = 0;
    shadnet_session(0);
    shadnet_session(1);
    assert(context_starts == 2);

    puts("np-shadnet-test: ok");
    return 0;
}
