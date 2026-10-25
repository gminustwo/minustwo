/* Online play by direct connection (MT_ONLINE=1): PSN as GODZILLA's VS mode needs it, between copies
 * of the port that reach each other by address instead of through Sony's servers. The player is signed
 * in as MT_ONLINE_NAME and the network is connected. Without MT_ONLINE nothing here is used and
 * runtime_services.c answers "signed out, no network" as before. Layouts follow the PS4 SDK as shadp2p
 * (a shadPS4 fork, GPL-2.0-or-later) documents them. */
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
typedef SOCKET LinkSocket;
#define close_socket closesocket
#define SEND_FLAGS 0
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int LinkSocket;
#define INVALID_SOCKET (-1)
#define close_socket close
#define SEND_FLAGS MSG_NOSIGNAL /* a closed connection is an error, not SIGPIPE */
#endif

#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
enum { NP_STATE_SIGNED_IN = 2, NET_CTL_STATE_IPOBTAINED = 3, NP_ONLINE_ID_MAX = 16 };

typedef struct { char data[NP_ONLINE_ID_MAX]; int8_t term; int8_t dummy[3]; } NpOnlineId;
typedef struct { NpOnlineId handle; uint8_t opt[8]; uint8_t reserved[8]; } NpId;
_Static_assert(sizeof(NpOnlineId) == 20 && sizeof(NpId) == 36, "PS4 NP id layouts");

static int online(void) {
    static int value = -1;
    if (value < 0) {
        const char *env = getenv("MT_ONLINE");
        value = env && *env == '1';
    }
    return value;
}

/* The Online ID: MT_ONLINE_NAME, as PSN allows it (letters, digits, - and _, at most 16). */
static void online_id(NpOnlineId *id) {
    const char *name = getenv("MT_ONLINE_NAME");
    memset(id, 0, sizeof(*id));
    size_t n = 0;
    for (const char *c = name && *name ? name : "MinusTwo"; *c && n < NP_ONLINE_ID_MAX; ++c) {
        if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_')
            id->data[n++] = *c;
    }
    if (!n) memcpy(id->data, "MinusTwo", n = 8);
}

static ABI int32_t np_get_state(int32_t user, int32_t *state) {
    (void)user;
    if (!state) return NP_INVALID_ARGUMENT;
    *state = NP_STATE_SIGNED_IN;
    return 0;
}

static ABI int32_t np_get_online_id(int32_t user, NpOnlineId *id) {
    (void)user;
    if (!id) return NP_INVALID_ARGUMENT;
    online_id(id);
    return 0;
}

static ABI int32_t np_get_np_id(int32_t user, NpId *id) {
    (void)user;
    if (!id) return NP_INVALID_ARGUMENT;
    memset(id, 0, sizeof(*id));
    online_id(&id->handle);
    return 0;
}

/* Account checks: PSN reachable, PlayStation Plus active, an adult without restrictions. Requests
 * finish at once (nothing goes over a network), so an async request's poll reports done, result OK. */
typedef struct { uint64_t size; int32_t user_id; uint8_t padding[4]; uint64_t features; uint8_t reserved[32]; } NpCheckPlusParameter;
typedef struct { uint8_t authorized; uint8_t reserved[32]; } NpCheckPlusResult;
typedef struct { uint8_t content_restriction, chat_restriction, user_generated_content_restriction; } NpParentalControlInfo;

static ABI int32_t np_check_availability(int32_t request, const NpOnlineId *id) {
    (void)request;
    return id ? 0 : NP_INVALID_ARGUMENT;
}

static ABI int32_t np_check_plus(int32_t request, const NpCheckPlusParameter *param, NpCheckPlusResult *result) {
    if (!request || !param || !result) return NP_INVALID_ARGUMENT;
    memset(result, 0, sizeof(*result));
    result->authorized = 1;
    return 0;
}

static ABI int32_t np_parental_control(int32_t request, const NpOnlineId *id, int8_t *age, NpParentalControlInfo *info) {
    (void)request;
    if (!id || !age || !info) return NP_INVALID_ARGUMENT;
    *age = 30;
    memset(info, 0, sizeof(*info));
    return 0;
}

static ABI int32_t np_poll_async(int32_t request, int32_t *result) {
    (void)request;
    if (!result) return NP_INVALID_ARGUMENT;
    *result = 0;
    return 0; /* finished */
}

static ABI int32_t np_game_presence(const NpOnlineId *id, int32_t *status) {
    if (!id || !status) return NP_INVALID_ARGUMENT;
    *status = 1; /* online */
    return 0;
}

static ABI int32_t netctl_get_state(int32_t *state) {
    if (!state) return (int32_t)0x80412104; /* NET_CTL_ERROR_INVALID_ADDR */
    *state = NET_CTL_STATE_IPOBTAINED;
    return 0;
}

/* ---- Matching2: contexts, requests, rooms -------------------------------------------------------
 * Results reach the game the PS4 way: through its callbacks, on a thread of their own ("NpMatching2"),
 * a moment after the call returned (the game files the request id before its answer can arrive). */
#define M2_NOT_INITIALIZED ((int32_t)0x80550C03)
#define M2_ALREADY_INITIALIZED ((int32_t)0x80550C02)
#define M2_CONTEXT_MAX ((int32_t)0x80550C04)
#define M2_CONTEXT_NOT_FOUND ((int32_t)0x80550C06)
#define M2_INVALID_ARGUMENT ((int32_t)0x80550C0A)
#define M2_ROOM_NOT_FOUND ((int32_t)0x80550C30)
enum {
    M2_REQUEST_GET_WORLD_INFO_LIST = 0x0002, M2_REQUEST_SET_ROOM_DATA_EXTERNAL = 0x0004,
    M2_REQUEST_CREATE_JOIN_ROOM = 0x0101, M2_REQUEST_JOIN_ROOM = 0x0102, M2_REQUEST_LEAVE_ROOM = 0x0103,
    M2_REQUEST_SEARCH_ROOM = 0x0106, M2_REQUEST_SET_ROOM_DATA_INTERNAL = 0x0109,
    M2_REQUEST_SET_ROOM_MEMBER_DATA_INTERNAL = 0x010B,
    M2_CONTEXT_EVENT_STARTED = 0x6F02, M2_CONTEXT_EVENT_STOPPED = 0x6F03, M2_CAUSE_CONTEXT_ACTION = 11,
    M2_SERVER_ID = 1, M2_WORLD_ID = 1, M2_MAX_MEMBERS = 4, M2_MAX_BIN = 512,
};

typedef ABI void (*ContextCallback)(uint16_t ctx, uint16_t event, uint8_t cause, int32_t error, void *arg);
typedef ABI void (*RequestCallback)(uint16_t ctx, uint32_t request, uint16_t event, int32_t error, const void *data, void *arg);
typedef ABI void (*RoomEventCallback)(uint16_t ctx, uint64_t room, uint16_t event, const void *data, void *arg);
typedef ABI void (*SignalingCallback)(uint16_t ctx, uint64_t room, uint16_t member, uint16_t event, int32_t error, void *arg);

typedef struct { RequestCallback callback; void *arg; uint32_t timeout; uint16_t app_id; uint8_t padding[2]; } M2RequestOptParam;
typedef struct { NpId *np_id; void *communication_id, *passphrase; uint32_t service_label; uint64_t size; } M2CreateContextParameter;
typedef struct { uint16_t id; uint8_t pad[6]; uint8_t *data; uint64_t size; } M2BinAttr;
typedef struct { uint16_t id; uint8_t padding[2]; uint32_t num; } M2IntAttr;
typedef struct M2World { struct M2World *next; uint32_t world_id, lobbies, max_lobby_members, lobby_members, rooms, room_members; uint8_t pad[3]; } M2World;
typedef struct { M2World *world; uint64_t count; } M2WorldListResponse;
typedef struct { uint64_t last_update; uint16_t member_id; uint8_t pad[6]; M2BinAttr attr; } M2RoomBinAttrInternal;
typedef struct { uint64_t last_update; M2BinAttr attr; } M2MemberBinAttrInternal;
typedef struct {
    uint16_t public_slots, private_slots, open_public_slots, open_private_slots, max_slot, server_id;
    uint32_t world_id; uint64_t lobby_id, room_id, password_slot_mask, joined_slot_mask;
    void *room_group; uint64_t room_groups; uint32_t flags; uint8_t pad[4];
    M2RoomBinAttrInternal *bin_attr; uint64_t bin_attrs;
} M2RoomDataInternal;
typedef struct M2MemberDataInternal {
    struct M2MemberDataInternal *next; uint64_t join_date; NpId np_id; uint8_t pad[4];
    uint16_t member_id; uint8_t team_id, nat_type; uint32_t flags;
    void *room_group; M2MemberBinAttrInternal *bin_attr; uint64_t bin_attrs;
} M2MemberDataInternal;
typedef struct { M2MemberDataInternal *members; uint64_t count; M2MemberDataInternal *me, *owner; } M2MemberDataInternalList;
typedef struct { const M2RoomDataInternal *room; M2MemberDataInternalList members; } M2JoinRoomResponse;
typedef struct M2RoomDataExternal {
    struct M2RoomDataExternal *next; uint16_t max_slot, cur_members; uint32_t flags; uint16_t server_id; uint8_t pad[2];
    uint32_t world_id; uint64_t lobby_id, room_id, password_slot_mask, joined_slot_mask;
    uint16_t public_slots, private_slots, open_public_slots, open_private_slots;
    NpId *owner; void *room_group; uint64_t room_groups;
    M2IntAttr *search_int_attr; uint64_t search_int_attrs; M2BinAttr *search_bin_attr; uint64_t search_bin_attrs;
    M2BinAttr *bin_attr; uint64_t bin_attrs;
} M2RoomDataExternal;
typedef struct { uint32_t start, total, results; uint8_t pad[4]; } M2Range;
typedef struct { M2Range range; M2RoomDataExternal *rooms; } M2SearchRoomResponse;
typedef struct {
    uint16_t max_slot; uint8_t team_id; uint8_t pad[5]; uint32_t flags; uint32_t world_id; uint64_t lobby_id;
    void *password, *password_slot_mask, *group_config; uint64_t group_configs; void *join_group_label;
    NpOnlineId *allowed; uint64_t alloweds; NpOnlineId *blocked; uint64_t blockeds;
    M2BinAttr *internal_bin; uint64_t internal_bins; M2IntAttr *search_int; uint64_t search_ints;
    M2BinAttr *search_bin; uint64_t search_bins; M2BinAttr *external_bin; uint64_t external_bins;
    M2BinAttr *member_bin; uint64_t member_bins; void *signaling;
} M2CreateJoinRoomRequest;
typedef struct {
    uint64_t room_id; void *password, *join_group_label; M2BinAttr *member_bin; uint64_t member_bins;
    uint8_t opt_data[16]; uint64_t opt_len; uint8_t team_id; uint8_t pad[3]; uint32_t flags;
    NpOnlineId *blocked; uint64_t blockeds;
} M2JoinRoomRequest;
typedef struct { uint64_t room_id; uint8_t opt_data[16]; uint64_t opt_len; } M2LeaveRoomRequest;
typedef struct {
    int32_t option; uint32_t world_id; uint64_t lobby_id; uint32_t range_start, range_max; uint32_t flags1, flags2;
    void *int_filter; uint64_t int_filters; void *bin_filter; uint64_t bin_filters; uint16_t *attr; uint64_t attrs;
} M2SearchRoomRequest;
typedef struct {
    uint64_t room_id; const M2IntAttr *search_int; uint64_t search_ints; const M2BinAttr *search_bin; uint64_t search_bins;
    const M2BinAttr *bin; uint64_t bins;
} M2SetRoomDataExternalRequest;
typedef struct {
    uint64_t room_id; uint32_t flag_filter, flag_attr; const M2BinAttr *bin; uint64_t bins;
    const void *password_config; uint64_t password_configs; const void *password_slot_mask;
    const uint16_t *owner_privilege_rank; uint64_t owner_privilege_ranks;
} M2SetRoomDataInternalRequest;
typedef struct {
    uint64_t room_id; uint16_t member_id; uint8_t team_id; uint8_t pad[5]; uint32_t flag_filter, flag_attr;
    const M2BinAttr *bin; uint64_t bins;
} M2SetRoomMemberDataInternalRequest;
_Static_assert(sizeof(M2CreateContextParameter) == 0x28 && sizeof(M2World) == 40 && sizeof(M2RoomDataInternal) == 88 &&
               sizeof(M2MemberDataInternal) == 0x58 && sizeof(M2CreateJoinRoomRequest) == 184 &&
               sizeof(M2JoinRoomRequest) == 0x58 && sizeof(M2RoomBinAttrInternal) == 40 &&
               sizeof(M2MemberBinAttrInternal) == 32, "PS4 Matching2 layouts");

typedef struct {
    int used, started; uint16_t id; NpId np_id;
    M2RequestOptParam defaults;
    RoomEventCallback room_cb; void *room_arg;
    SignalingCallback signaling_cb; void *signaling_arg;
} M2Context;

/* A binary attribute's value as the room keeps it. */
typedef struct { uint16_t id; uint32_t size; uint64_t updated; uint16_t by; uint8_t data[M2_MAX_BIN]; } StoredBin;
typedef struct { int used; uint16_t member_id; NpId np_id; uint8_t team; uint32_t flags; uint64_t joined; StoredBin bin[2]; } RoomMember;
typedef struct {
    int active; uint64_t room_id; uint16_t max_slot, owner, next_member; uint32_t flags;
    RoomMember members[M2_MAX_MEMBERS];
    StoredBin internal[2], external[2], search_bin[1];
    M2IntAttr search_int[8]; int search_ints;
} Room;

static pthread_mutex_t m2_lock = PTHREAD_MUTEX_INITIALIZER;
static int m2_initialized;
static M2Context contexts[4];
static ContextCallback context_cb;
static void *context_arg;
static uint32_t next_request = 1;
static Room room; /* the room this player is in (direct connection: one at a time) */

static uint64_t ticks_now(void) { return (uint64_t)time(NULL) * 1000000ull + 62135596800ull * 1000000ull; } /* RtcTick: µs since year 1 */

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
#endif
}

/* ---- events ---- */
enum { EVENT_CONTEXT, EVENT_REQUEST, EVENT_ROOM, EVENT_SIGNALING };
typedef struct Event {
    struct Event *next; int kind; uint16_t ctx, event, member; uint8_t cause; int32_t error;
    uint32_t request; uint64_t room; void *data; void *callback, *arg;
} Event;
static Event *queue_head, *queue_tail;
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;
static int event_thread_started;

static void post(Event e) {
    Event *p = malloc(sizeof(*p));
    if (!p) { free(e.data); return; }
    *p = e;
    p->next = NULL;
    pthread_mutex_lock(&queue_lock);
    if (queue_tail) queue_tail->next = p; else queue_head = p;
    queue_tail = p;
    pthread_cond_signal(&queue_cond);
    pthread_mutex_unlock(&queue_lock);
}

static void *event_main(void *unused) {
    (void)unused;
    runtime_thread_attach_host("NpMatching2");
    for (;;) {
        pthread_mutex_lock(&queue_lock);
        while (!queue_head) pthread_cond_wait(&queue_cond, &queue_lock);
        Event *e = queue_head;
        queue_head = e->next;
        if (!queue_head) queue_tail = NULL;
        pthread_mutex_unlock(&queue_lock);
        sleep_ms(10);
        switch (e->kind) {
        case EVENT_CONTEXT: ((ContextCallback)e->callback)(e->ctx, e->event, e->cause, e->error, e->arg); break;
        case EVENT_REQUEST: ((RequestCallback)e->callback)(e->ctx, e->request, e->event, e->error, e->data, e->arg); break;
        case EVENT_ROOM: ((RoomEventCallback)e->callback)(e->ctx, e->room, e->event, e->data, e->arg); break;
        case EVENT_SIGNALING:
            ((SignalingCallback)e->callback)(e->ctx, e->room, e->member, e->event, e->error, e->arg);
            break;
        }
        free(e->data);
        free(e);
    }
    return NULL;
}

static M2Context *find_context(uint16_t id) {
    for (size_t i = 0; i < sizeof(contexts) / sizeof(*contexts); ++i)
        if (contexts[i].used && contexts[i].id == id) return &contexts[i];
    return NULL;
}

/* A request's answer: `data` (one malloc block, freed after the callback) to the request's callback,
 * or the context's default one. */
static int32_t answer(M2Context *c, const M2RequestOptParam *opt, uint32_t *request_id, uint16_t event, int32_t error,
                      void *data) {
    RequestCallback cb = opt ? opt->callback : c->defaults.callback;
    void *arg = opt ? opt->arg : c->defaults.arg;
    pthread_mutex_lock(&m2_lock);
    uint32_t id = next_request++;
    pthread_mutex_unlock(&m2_lock);
    if (request_id) *request_id = id;
    if (cb) post((Event){.kind = EVENT_REQUEST, .ctx = c->id, .request = id, .event = event, .error = error,
                         .data = data, .callback = (void *)cb, .arg = arg});
    else free(data);
    return 0;
}

/* ---- room snapshots in the PS4 layouts (one block each, pointers into itself) ---- */
static void store_bin(StoredBin *slot, const M2BinAttr *attr, uint16_t by) {
    slot->id = attr->id;
    slot->size = attr->size > M2_MAX_BIN ? M2_MAX_BIN : (uint32_t)attr->size;
    if (attr->data && slot->size) memcpy(slot->data, attr->data, slot->size);
    slot->updated = ticks_now();
    slot->by = by;
}

static int members_in(const Room *r) {
    int n = 0;
    for (int i = 0; i < M2_MAX_MEMBERS; ++i) n += r->members[i].used;
    return n;
}

/* Room data internal and the member list, for CreateJoinRoom/JoinRoom answers (me: this player), in
 * a zeroed block of SNAPSHOT_SIZE bytes. */
#define SNAPSHOT_SIZE (sizeof(M2JoinRoomResponse) + sizeof(M2RoomDataInternal) + 2 * sizeof(M2RoomBinAttrInternal) + \
                       M2_MAX_MEMBERS * (sizeof(M2MemberDataInternal) + 2 * sizeof(M2MemberBinAttrInternal)) + \
                       (2 + 2 * M2_MAX_MEMBERS) * M2_MAX_BIN)
static M2JoinRoomResponse *snapshot_into(uint8_t *block, const Room *r, uint16_t me) {
    uint8_t *p = block;
    M2JoinRoomResponse *response = (M2JoinRoomResponse *)p; p += sizeof(*response);
    M2RoomDataInternal *data = (M2RoomDataInternal *)p; p += sizeof(*data);
    M2RoomBinAttrInternal *bins = (M2RoomBinAttrInternal *)p; p += 2 * sizeof(*bins);
    M2MemberDataInternal *members = (M2MemberDataInternal *)p; p += M2_MAX_MEMBERS * sizeof(*members);
    M2MemberBinAttrInternal *member_bins = (M2MemberBinAttrInternal *)p; p += 2 * M2_MAX_MEMBERS * sizeof(*member_bins);
    int count = members_in(r);
    data->public_slots = data->max_slot = r->max_slot;
    data->open_public_slots = (uint16_t)(r->max_slot > count ? r->max_slot - count : 0);
    data->server_id = M2_SERVER_ID;
    data->world_id = M2_WORLD_ID;
    data->room_id = r->room_id;
    data->flags = r->flags;
    for (int i = 0; i < 2; ++i) {
        if (!r->internal[i].id) continue;
        M2RoomBinAttrInternal *b = &bins[data->bin_attrs++];
        b->last_update = r->internal[i].updated;
        b->member_id = r->internal[i].by;
        b->attr.id = r->internal[i].id;
        b->attr.size = r->internal[i].size;
        b->attr.data = p;
        memcpy(p, r->internal[i].data, r->internal[i].size);
        p += M2_MAX_BIN;
    }
    data->bin_attr = data->bin_attrs ? bins : NULL;
    M2MemberDataInternal *previous = NULL;
    int n = 0;
    for (int i = 0; i < M2_MAX_MEMBERS; ++i) {
        const RoomMember *m = &r->members[i];
        if (!m->used) continue;
        M2MemberDataInternal *out = &members[n++];
        data->joined_slot_mask |= 1ull << (m->member_id - 1);
        out->join_date = m->joined;
        out->np_id = m->np_id;
        out->member_id = m->member_id;
        out->team_id = m->team;
        out->nat_type = 1;
        out->flags = m->flags | (m->member_id == r->owner ? 0x80000000u : 0);
        M2MemberBinAttrInternal *mb = &member_bins[2 * i];
        for (int k = 0; k < 2; ++k) {
            if (!m->bin[k].id) continue;
            M2MemberBinAttrInternal *b = &mb[out->bin_attrs++];
            b->last_update = m->bin[k].updated;
            b->attr.id = m->bin[k].id;
            b->attr.size = m->bin[k].size;
            b->attr.data = p;
            memcpy(p, m->bin[k].data, m->bin[k].size);
            p += M2_MAX_BIN;
        }
        out->bin_attr = out->bin_attrs ? mb : NULL;
        if (previous) previous->next = out;
        previous = out;
        if (m->member_id == me) response->members.me = out;
        if (m->member_id == r->owner) response->members.owner = out;
    }
    response->room = data;
    response->members.members = n ? members : NULL;
    response->members.count = (uint64_t)n;
    return response;
}

static M2JoinRoomResponse *room_snapshot(const Room *r, uint16_t me) {
    uint8_t *block = calloc(1, SNAPSHOT_SIZE);
    return block ? snapshot_into(block, r, me) : NULL;
}

/* ---- room events in the PS4 layouts ---- */
enum {
    M2_ROOM_EVENT_MEMBER_JOINED = 0x1101, M2_ROOM_EVENT_MEMBER_LEFT = 0x1102, M2_ROOM_EVENT_ROOM_DESTROYED = 0x1104,
    M2_ROOM_EVENT_UPDATED_ROOM_DATA_INTERNAL = 0x1106, M2_ROOM_EVENT_UPDATED_ROOM_MEMBER_DATA_INTERNAL = 0x1107,
    M2_SIGNALING_EVENT_DEAD = 0x5101, M2_SIGNALING_EVENT_ESTABLISHED = 0x5102,
    M2_CAUSE_LEAVE_ACTION = 1, M2_CAUSE_MEMBER_DISAPPEARED = 5, M2_CAUSE_CONNECTION_ERROR = 7,
};
typedef struct { M2MemberDataInternal *member; uint8_t cause; uint8_t padding[7]; uint8_t opt_data[16]; uint64_t opt_len; } M2RoomMemberUpdate;
typedef struct { uint8_t cause; uint8_t padding[3]; int32_t error; uint8_t opt_data[16]; uint64_t opt_len; } M2RoomUpdate;
typedef struct {
    M2RoomDataInternal *room; uint32_t *new_flags, *previous_flags; uint64_t *new_password_mask, *previous_password_mask;
    void **groups; uint64_t group_count; M2RoomBinAttrInternal **bins; uint64_t bin_count;
} M2RoomDataInternalUpdate;
typedef struct {
    M2MemberDataInternal *member; uint32_t *new_flags, *previous_flags; uint8_t *new_team;
    M2MemberBinAttrInternal **bins; uint64_t bin_count;
} M2MemberDataInternalUpdate;

static RoomMember *find_member(Room *r, uint16_t id) {
    for (int i = 0; i < M2_MAX_MEMBERS; ++i)
        if (r->members[i].used && r->members[i].member_id == id) return &r->members[i];
    return NULL;
}

/* One member in the PS4 layout, bins after it (block: the member, 2 bin entries, 2 data buffers). */
static void fill_member(M2MemberDataInternal *out, M2MemberBinAttrInternal *bins, uint8_t *data, const RoomMember *m,
                        uint16_t owner) {
    out->join_date = m->joined;
    out->np_id = m->np_id;
    out->member_id = m->member_id;
    out->team_id = m->team;
    out->nat_type = 1;
    out->flags = m->flags | (m->member_id == owner ? 0x80000000u : 0);
    for (int k = 0; k < 2; ++k) {
        if (!m->bin[k].id) continue;
        M2MemberBinAttrInternal *b = &bins[out->bin_attrs++];
        b->last_update = m->bin[k].updated;
        b->attr.id = m->bin[k].id;
        b->attr.size = m->bin[k].size;
        b->attr.data = data + k * M2_MAX_BIN;
        memcpy(b->attr.data, m->bin[k].data, m->bin[k].size);
    }
    out->bin_attr = out->bin_attrs ? bins : NULL;
}

static M2RoomMemberUpdate *member_event(const Room *r, uint16_t id, uint8_t cause) {
    const RoomMember *m = NULL;
    for (int i = 0; i < M2_MAX_MEMBERS; ++i)
        if (r->members[i].member_id == id) m = &r->members[i]; /* also a member that just left */
    uint8_t *block = calloc(1, sizeof(M2RoomMemberUpdate) + sizeof(M2MemberDataInternal) + 2 * sizeof(M2MemberBinAttrInternal) +
                                   2 * M2_MAX_BIN);
    if (!block || !m) { free(block); return NULL; }
    M2RoomMemberUpdate *update = (M2RoomMemberUpdate *)block;
    M2MemberDataInternal *member = (M2MemberDataInternal *)(update + 1);
    M2MemberBinAttrInternal *bins = (M2MemberBinAttrInternal *)(member + 1);
    fill_member(member, bins, (uint8_t *)(bins + 2), m, r->owner);
    update->member = member;
    update->cause = cause;
    return update;
}

static M2RoomDataInternalUpdate *room_data_event(const Room *r, uint32_t previous_flags) {
    /* One block: the update, its flags and pointer list, then the room's snapshot. */
    size_t head = sizeof(M2RoomDataInternalUpdate) + 2 * sizeof(uint32_t) + 2 * sizeof(void *);
    uint8_t *block = calloc(1, head + SNAPSHOT_SIZE);
    if (!block) return NULL;
    M2RoomDataInternalUpdate *update = (M2RoomDataInternalUpdate *)block;
    uint32_t *flags = (uint32_t *)(update + 1);
    M2RoomBinAttrInternal **bins = (M2RoomBinAttrInternal **)(flags + 2);
    M2JoinRoomResponse *snapshot = snapshot_into(block + head, r, 0);
    flags[0] = r->flags;
    flags[1] = previous_flags;
    update->room = (M2RoomDataInternal *)snapshot->room;
    update->new_flags = &flags[0];
    update->previous_flags = &flags[1];
    for (uint64_t i = 0; i < update->room->bin_attrs; ++i) bins[i] = &update->room->bin_attr[i];
    update->bins = update->room->bin_attrs ? bins : NULL;
    update->bin_count = update->room->bin_attrs;
    return update;
}

static M2MemberDataInternalUpdate *member_data_event(const Room *r, uint16_t id, uint32_t previous_flags) {
    const RoomMember *m = NULL;
    for (int i = 0; i < M2_MAX_MEMBERS; ++i)
        if (r->members[i].used && r->members[i].member_id == id) m = &r->members[i];
    if (!m) return NULL;
    uint8_t *block = calloc(1, sizeof(M2MemberDataInternalUpdate) + sizeof(M2MemberDataInternal) +
                                   2 * sizeof(M2MemberBinAttrInternal) + 2 * M2_MAX_BIN + 2 * sizeof(uint32_t) + 8 +
                                   2 * sizeof(void *));
    if (!block) return NULL;
    M2MemberDataInternalUpdate *update = (M2MemberDataInternalUpdate *)block;
    M2MemberDataInternal *member = (M2MemberDataInternal *)(update + 1);
    M2MemberBinAttrInternal *bins = (M2MemberBinAttrInternal *)(member + 1);
    uint8_t *data = (uint8_t *)(bins + 2);
    uint32_t *flags = (uint32_t *)(data + 2 * M2_MAX_BIN);
    uint8_t *team = (uint8_t *)(flags + 2);
    M2MemberBinAttrInternal **bin_list = (M2MemberBinAttrInternal **)(team + 8);
    fill_member(member, bins, data, m, r->owner);
    flags[0] = member->flags;
    flags[1] = previous_flags;
    *team = m->team;
    for (uint64_t i = 0; i < member->bin_attrs; ++i) bin_list[i] = &member->bin_attr[i];
    update->member = member;
    update->new_flags = &flags[0];
    update->previous_flags = &flags[1];
    update->new_team = team;
    update->bins = member->bin_attrs ? bin_list : NULL;
    update->bin_count = member->bin_attrs;
    return update;
}

/* The context the room belongs to: its callbacks get the room and signaling events. */
static uint16_t room_context;

static void post_room_event(uint16_t event, void *data) {
    M2Context *c = find_context(room_context);
    if (!c || !c->room_cb) { free(data); return; }
    post((Event){.kind = EVENT_ROOM, .ctx = c->id, .room = room.room_id, .event = event, .data = data,
                 .callback = (void *)c->room_cb, .arg = c->room_arg});
}

static void post_signaling(uint16_t member, uint16_t event) {
    M2Context *c = find_context(room_context);
    if (!c || !c->signaling_cb) return;
    post((Event){.kind = EVENT_SIGNALING, .ctx = c->id, .room = room.room_id, .member = member, .event = event,
                 .callback = (void *)c->signaling_cb, .arg = c->signaling_arg});
}

/* ---- the direct connection ------------------------------------------------------------------------
 * The host (no MT_ONLINE_JOIN) listens on MT_ONLINE_PORT (TCP, default 31470) while it has a room; a
 * joiner (MT_ONLINE_JOIN=address[:port]) connects to it. Messages: an 8-byte header (payload size,
 * type, protocol version) and the payload. The room travels whole: both ends are this port. */
enum { LINK_PORT = 31470, LINK_VERSION = 1, MAX_PEERS = 3 };
enum { MSG_HELLO = 1, MSG_QUERY, MSG_ROOM, MSG_JOIN, MSG_JOINED, MSG_REFUSED, MSG_UPDATE, MSG_MEMBER_SET, MSG_LEAVE,
       MSG_DESTROYED, MSG_RUDP };
typedef struct { uint32_t size; uint16_t type, version; } LinkHeader;
typedef struct { uint64_t room_id; uint8_t team; uint8_t pad[3]; uint32_t flags; StoredBin bin[2]; } JoinMessage;
typedef struct { uint16_t member_id; uint8_t pad[2]; int32_t error; Room room; } JoinedMessage;
typedef struct { uint16_t event, member_id; uint32_t previous_flags; uint8_t cause; uint8_t pad[7]; Room room; } UpdateMessage;
typedef struct {
    uint16_t member_id; uint8_t team; uint8_t pad; uint32_t flag_filter, flag_attr, bins; StoredBin bin[2];
} MemberSetMessage;
typedef struct {
    int used; LinkSocket socket; uint16_t member_id; NpId np_id; uint32_t address; pthread_mutex_t send_lock;
} Peer;

static Peer peers[MAX_PEERS]; /* the host: its joiners; a joiner: peers[0], the host */
static LinkSocket listener = INVALID_SOCKET;
static uint16_t my_member;
/* A joiner's replies from the host (ROOM, JOINED, REFUSED), handed from the reading thread. */
static pthread_cond_t reply_cond = PTHREAD_COND_INITIALIZER;
static uint16_t reply_type;
static void *reply_data;

static const char *join_address(void) {
    const char *join = getenv("MT_ONLINE_JOIN");
    return join && *join ? join : NULL;
}

static uint16_t link_port(void) {
    const char *port = getenv("MT_ONLINE_PORT");
    int value = port ? atoi(port) : 0;
    return (uint16_t)(value > 0 && value < 65536 ? value : LINK_PORT);
}

static void sockets_init(void) {
#ifdef _WIN32
    static int done;
    if (!done) {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
        done = 1;
    }
#endif
}

static int send_all(LinkSocket s, const void *data, size_t size) {
    const char *p = data;
    while (size) {
        int n = send(s, p, (int)(size > 1 << 30 ? 1 << 30 : size), SEND_FLAGS);
        if (n <= 0) return -1;
        p += n;
        size -= (size_t)n;
    }
    return 0;
}

static int recv_all(LinkSocket s, void *data, size_t size) {
    char *p = data;
    while (size) {
        int n = recv(s, p, (int)(size > 1 << 30 ? 1 << 30 : size), 0);
        if (n <= 0) return -1;
        p += n;
        size -= (size_t)n;
    }
    return 0;
}

static int link_send(Peer *p, uint16_t type, const void *payload, uint32_t size) {
    LinkHeader h = {size, type, LINK_VERSION};
    pthread_mutex_lock(&p->send_lock);
    int failed = send_all(p->socket, &h, sizeof(h)) || (size && send_all(p->socket, payload, size));
    pthread_mutex_unlock(&p->send_lock);
    return failed ? -1 : 0;
}

/* The next message (payload malloc'ed, NULL for none), or -1 when the connection is gone. */
static int link_receive(LinkSocket s, LinkHeader *h, void **payload) {
    *payload = NULL;
    if (recv_all(s, h, sizeof(*h)) || h->version != LINK_VERSION || h->size > (1u << 24)) return -1;
    if (!h->size) return 0;
    *payload = malloc(h->size);
    if (!*payload || recv_all(s, *payload, h->size)) { free(*payload); *payload = NULL; return -1; }
    return 0;
}

static void broadcast(uint16_t type, const void *payload, uint32_t size, const Peer *except) {
    for (int i = 0; i < MAX_PEERS; ++i)
        if (peers[i].used && &peers[i] != except) link_send(&peers[i], type, payload, size);
}

/* The room changed (under m2_lock): every other member is told, with what changed. */
static void send_update(uint16_t event, uint16_t member, uint32_t previous_flags, uint8_t cause, const Peer *except) {
    UpdateMessage *m = malloc(sizeof(*m));
    if (!m) return;
    memset(m, 0, sizeof(*m));
    m->event = event;
    m->member_id = member;
    m->previous_flags = previous_flags;
    m->cause = cause;
    m->room = room;
    broadcast(MSG_UPDATE, m, sizeof(*m), except);
    free(m);
}

static Peer *new_peer(LinkSocket s, uint32_t address) {
    for (int i = 0; i < MAX_PEERS; ++i) {
        if (peers[i].used) continue;
        memset(&peers[i], 0, sizeof(peers[i]));
        pthread_mutex_init(&peers[i].send_lock, NULL);
        peers[i].used = 1;
        peers[i].socket = s;
        peers[i].address = address;
        return &peers[i];
    }
    return NULL;
}

static void drop_peer(Peer *p) {
    close_socket(p->socket);
    p->used = 0;
}

static void member_left(uint16_t member, uint8_t cause) {
    RoomMember *m = find_member(&room, member);
    if (!m) return;
    m->used = 0; /* member_event still finds it by id */
    printf("Online: %.16s left the room\n", m->np_id.handle.data);
    post_room_event(M2_ROOM_EVENT_MEMBER_LEFT, member_event(&room, member, cause));
    post_signaling(member, M2_SIGNALING_EVENT_DEAD);
    send_update(M2_ROOM_EVENT_MEMBER_LEFT, member, 0, cause, NULL);
    m->member_id = 0;
}

/* The host's side of one joiner. */
static void *host_peer_main(void *arg) {
    Peer *p = arg;
    runtime_thread_attach_host("NpLinkPeer");
    LinkHeader h;
    void *payload;
    while (!link_receive(p->socket, &h, &payload)) {
        pthread_mutex_lock(&m2_lock);
        switch (h.type) {
        case MSG_HELLO:
            if (payload && h.size >= sizeof(NpId)) memcpy(&p->np_id, payload, sizeof(NpId));
            break;
        case MSG_QUERY: {
            Room *copy = malloc(sizeof(Room));
            if (copy) { *copy = room; link_send(p, MSG_ROOM, copy, sizeof(Room)); free(copy); }
            break;
        }
        case MSG_JOIN: {
            const JoinMessage *j = payload;
            JoinedMessage *reply = calloc(1, sizeof(*reply));
            if (!reply) break;
            RoomMember *slot = NULL;
            for (int i = 0; i < M2_MAX_MEMBERS && !slot; ++i)
                if (!room.members[i].used) slot = &room.members[i];
            if (!j || h.size < sizeof(*j) || !room.active || j->room_id != room.room_id) reply->error = M2_ROOM_NOT_FOUND;
            else if (!slot || members_in(&room) >= room.max_slot) reply->error = (int32_t)0x80550C31; /* ROOM_FULL */
            if (!reply->error) {
                memset(slot, 0, sizeof(*slot));
                slot->used = 1;
                slot->member_id = room.next_member++;
                slot->np_id = p->np_id;
                slot->team = j->team;
                slot->flags = j->flags;
                slot->joined = ticks_now();
                memcpy(slot->bin, j->bin, sizeof(slot->bin));
                p->member_id = slot->member_id;
                reply->member_id = slot->member_id;
                reply->room = room;
                link_send(p, MSG_JOINED, reply, sizeof(*reply));
                printf("Online: %.16s joined the room as member %u\n", p->np_id.handle.data, slot->member_id);
                post_room_event(M2_ROOM_EVENT_MEMBER_JOINED, member_event(&room, slot->member_id, 0));
                post_signaling(slot->member_id, M2_SIGNALING_EVENT_ESTABLISHED);
                send_update(M2_ROOM_EVENT_MEMBER_JOINED, slot->member_id, 0, 0, p);
            } else {
                link_send(p, MSG_REFUSED, reply, sizeof(*reply));
            }
            free(reply);
            break;
        }
        case MSG_MEMBER_SET: {
            const MemberSetMessage *s = payload;
            RoomMember *m = s && h.size >= sizeof(*s) ? find_member(&room, s->member_id) : NULL;
            if (!m) break;
            uint32_t previous = m->flags;
            if (s->team) m->team = s->team;
            m->flags = (m->flags & ~s->flag_filter) | (s->flag_attr & s->flag_filter);
            for (uint32_t k = 0; k < s->bins && k < 2; ++k) m->bin[k] = s->bin[k];
            post_room_event(M2_ROOM_EVENT_UPDATED_ROOM_MEMBER_DATA_INTERNAL, member_data_event(&room, m->member_id, previous));
            send_update(M2_ROOM_EVENT_UPDATED_ROOM_MEMBER_DATA_INTERNAL, m->member_id, previous, 0, p);
            break;
        }
        case MSG_LEAVE:
            if (p->member_id) member_left(p->member_id, M2_CAUSE_LEAVE_ACTION);
            p->member_id = 0;
            break;
        case MSG_RUDP:
            break; /* the game's traffic: not routed yet */
        }
        pthread_mutex_unlock(&m2_lock);
        free(payload);
    }
    pthread_mutex_lock(&m2_lock);
    if (p->member_id) member_left(p->member_id, M2_CAUSE_MEMBER_DISAPPEARED);
    drop_peer(p);
    pthread_mutex_unlock(&m2_lock);
    return NULL;
}

static void *listener_main(void *unused) {
    (void)unused;
    runtime_thread_attach_host("NpLinkListen");
    for (;;) {
        struct sockaddr_in from;
        socklen_t length = sizeof(from);
        LinkSocket s = accept(listener, (struct sockaddr *)&from, &length);
        if (s == INVALID_SOCKET) break;
        int on = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));
        pthread_mutex_lock(&m2_lock);
        Peer *p = new_peer(s, from.sin_addr.s_addr);
        if (p) {
            M2Context *c = find_context(room_context);
            NpId me;
            memset(&me, 0, sizeof(me));
            if (c) me = c->np_id;
            link_send(p, MSG_HELLO, &me, sizeof(me));
        }
        pthread_mutex_unlock(&m2_lock);
        if (!p) { close_socket(s); continue; }
        printf("Online: a player connected from %s\n", inet_ntoa(from.sin_addr));
        pthread_t thread;
        pthread_create(&thread, NULL, host_peer_main, p);
        pthread_detach(thread);
    }
    return NULL;
}

static void start_listener(void) {
    if (join_address() || listener != INVALID_SOCKET) return;
    sockets_init();
    LinkSocket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return;
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));
    struct sockaddr_in at;
    memset(&at, 0, sizeof(at));
    at.sin_family = AF_INET;
    at.sin_port = htons(link_port());
    /* MT_ONLINE_BIND: one address only (127.0.0.1 for two copies on one PC, no firewall prompt). */
    const char *bind_to = getenv("MT_ONLINE_BIND");
    at.sin_addr.s_addr = bind_to && *bind_to ? inet_addr(bind_to) : htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&at, sizeof(at)) || listen(s, 4)) {
        printf("Online: cannot listen on port %u (another program uses it?)\n", link_port());
        close_socket(s);
        return;
    }
    listener = s;
    printf("Online: hosting, waiting for players on TCP port %u\n", link_port());
    pthread_t thread;
    pthread_create(&thread, NULL, listener_main, NULL);
    pthread_detach(thread);
}

/* ---- the joiner's side ---- */
static void deliver_reply(uint16_t type, void *payload) {
    free(reply_data);
    reply_type = type;
    reply_data = payload;
    pthread_cond_broadcast(&reply_cond);
}

/* Waits (m2_lock held) for a reply of `type` (or `other`) for up to `ms`; the payload is the caller's. */
static void *await_reply(uint16_t type, uint16_t other, int ms, uint16_t *got) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ms / 1000;
    until.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) { until.tv_sec += 1; until.tv_nsec -= 1000000000L; }
    while (reply_type != type && reply_type != other)
        if (pthread_cond_timedwait(&reply_cond, &m2_lock, &until)) return NULL;
    void *data = reply_data;
    if (got) *got = reply_type;
    reply_data = NULL;
    reply_type = 0;
    return data;
}

static void *joiner_main(void *arg) {
    Peer *p = arg;
    runtime_thread_attach_host("NpLinkHost");
    LinkHeader h;
    void *payload;
    while (!link_receive(p->socket, &h, &payload)) {
        pthread_mutex_lock(&m2_lock);
        switch (h.type) {
        case MSG_HELLO:
            if (payload && h.size >= sizeof(NpId)) memcpy(&p->np_id, payload, sizeof(NpId));
            break;
        case MSG_ROOM: case MSG_JOINED: case MSG_REFUSED:
            deliver_reply(h.type, payload);
            payload = NULL;
            break;
        case MSG_UPDATE: {
            const UpdateMessage *u = payload;
            if (!u || h.size < sizeof(*u) || !room.active || u->room.room_id != room.room_id) break;
            room = u->room;
            if (u->event == M2_ROOM_EVENT_MEMBER_JOINED) {
                post_room_event(u->event, member_event(&room, u->member_id, 0));
                post_signaling(u->member_id, M2_SIGNALING_EVENT_ESTABLISHED);
            } else if (u->event == M2_ROOM_EVENT_MEMBER_LEFT) {
                post_room_event(u->event, member_event(&room, u->member_id, u->cause));
                post_signaling(u->member_id, M2_SIGNALING_EVENT_DEAD);
            } else if (u->event == M2_ROOM_EVENT_UPDATED_ROOM_DATA_INTERNAL) {
                post_room_event(u->event, room_data_event(&room, u->previous_flags));
            } else if (u->event == M2_ROOM_EVENT_UPDATED_ROOM_MEMBER_DATA_INTERNAL) {
                post_room_event(u->event, member_data_event(&room, u->member_id, u->previous_flags));
            }
            break;
        }
        case MSG_DESTROYED:
            if (room.active) {
                printf("Online: the host closed the room\n");
                M2RoomUpdate *update = calloc(1, sizeof(*update));
                if (update) update->cause = M2_CAUSE_LEAVE_ACTION;
                post_room_event(M2_ROOM_EVENT_ROOM_DESTROYED, update);
                post_signaling(room.owner, M2_SIGNALING_EVENT_DEAD);
                memset(&room, 0, sizeof(room));
            }
            break;
        case MSG_RUDP:
            break; /* the game's traffic: not routed yet */
        }
        pthread_mutex_unlock(&m2_lock);
        free(payload);
    }
    pthread_mutex_lock(&m2_lock);
    printf("Online: the connection to the host closed\n");
    if (room.active) {
        M2RoomUpdate *update = calloc(1, sizeof(*update));
        if (update) update->cause = M2_CAUSE_CONNECTION_ERROR;
        post_room_event(M2_ROOM_EVENT_ROOM_DESTROYED, update);
        memset(&room, 0, sizeof(room));
    }
    deliver_reply(MSG_REFUSED, NULL);
    drop_peer(p);
    pthread_mutex_unlock(&m2_lock);
    return NULL;
}

/* The connection to the host (m2_lock held), made on first use. */
static Peer *host_link(const NpId *me) {
    if (peers[0].used) return &peers[0];
    const char *address = join_address();
    if (!address) return NULL;
    sockets_init();
    char host[256];
    snprintf(host, sizeof(host), "%s", address);
    char *colon = strrchr(host, ':');
    char port[16];
    snprintf(port, sizeof(port), "%u", colon ? (unsigned)atoi(colon + 1) : link_port());
    if (colon) *colon = 0;
    struct addrinfo hints, *found = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    pthread_mutex_unlock(&m2_lock); /* name lookup and connect can take a while */
    LinkSocket s = INVALID_SOCKET;
    uint32_t ip = 0;
    if (!getaddrinfo(host, port, &hints, &found) && found) {
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s != INVALID_SOCKET && connect(s, found->ai_addr, (socklen_t)found->ai_addrlen)) { close_socket(s); s = INVALID_SOCKET; }
        if (s != INVALID_SOCKET) ip = ((struct sockaddr_in *)found->ai_addr)->sin_addr.s_addr;
        freeaddrinfo(found);
    }
    pthread_mutex_lock(&m2_lock);
    if (s == INVALID_SOCKET) return NULL;
    if (peers[0].used) { close_socket(s); return &peers[0]; }
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));
    Peer *p = new_peer(s, ip);
    link_send(p, MSG_HELLO, me, sizeof(*me));
    printf("Online: connected to the host at %s:%s\n", host, port);
    pthread_t thread;
    pthread_create(&thread, NULL, joiner_main, p);
    pthread_detach(thread);
    return p;
}

/* Int search filters (operators: 1 ==, 2 !=, 3 <, 4 <=, 5 >, 6 >=) against the room's attributes. */
typedef struct { uint8_t op; uint8_t padding[7]; M2IntAttr attr; } M2IntSearchFilter;
static int room_matches(const Room *r, const M2IntSearchFilter *filters, uint64_t count) {
    if (!r->active || members_in(r) >= r->max_slot) return 0;
    for (uint64_t i = 0; i < count; ++i) {
        const M2IntSearchFilter *f = &filters[i];
        int found = 0;
        uint32_t value = 0;
        for (int k = 0; k < r->search_ints; ++k)
            if (r->search_int[k].id == f->attr.id) { found = 1; value = r->search_int[k].num; }
        if (!found) return 0;
        uint32_t want = f->attr.num;
        int ok = f->op == 1 ? value == want : f->op == 2 ? value != want : f->op == 3 ? value < want
               : f->op == 4 ? value <= want : f->op == 5 ? value > want : f->op == 6 ? value >= want : 1;
        if (!ok) return 0;
    }
    return 1;
}

/* A room as SearchRoom answers it. */
static M2SearchRoomResponse *search_response(const Room *r) {
    size_t size = sizeof(M2SearchRoomResponse) + sizeof(M2RoomDataExternal) + sizeof(NpId) + 8 * sizeof(M2IntAttr) +
                  3 * sizeof(M2BinAttr) + 3 * M2_MAX_BIN;
    uint8_t *block = calloc(1, size);
    if (!block) return NULL;
    M2SearchRoomResponse *response = (M2SearchRoomResponse *)block;
    response->range.start = 1;
    if (!r) return response;
    M2RoomDataExternal *ext = (M2RoomDataExternal *)(response + 1);
    NpId *owner = (NpId *)(ext + 1);
    M2IntAttr *ints = (M2IntAttr *)(owner + 1);
    M2BinAttr *bins = (M2BinAttr *)(ints + 8);
    uint8_t *data = (uint8_t *)(bins + 3);
    int count = members_in(r);
    ext->max_slot = ext->public_slots = r->max_slot;
    ext->cur_members = (uint16_t)count;
    ext->open_public_slots = (uint16_t)(r->max_slot > count ? r->max_slot - count : 0);
    ext->flags = r->flags;
    ext->server_id = M2_SERVER_ID;
    ext->world_id = M2_WORLD_ID;
    ext->room_id = r->room_id;
    for (int i = 0; i < M2_MAX_MEMBERS; ++i) {
        if (!r->members[i].used) continue;
        ext->joined_slot_mask |= 1ull << (r->members[i].member_id - 1);
        if (r->members[i].member_id == r->owner) *owner = r->members[i].np_id;
    }
    ext->owner = owner;
    for (int i = 0; i < r->search_ints; ++i) ints[i] = r->search_int[i];
    ext->search_int_attr = r->search_ints ? ints : NULL;
    ext->search_int_attrs = (uint64_t)r->search_ints;
    const StoredBin *stored[3] = {&r->search_bin[0], &r->external[0], &r->external[1]};
    for (int i = 0; i < 3; ++i) {
        if (!stored[i]->id) continue;
        bins[i].id = stored[i]->id;
        bins[i].size = stored[i]->size;
        bins[i].data = data + i * M2_MAX_BIN;
        memcpy(bins[i].data, stored[i]->data, stored[i]->size);
    }
    if (bins[0].id) { ext->search_bin_attr = &bins[0]; ext->search_bin_attrs = 1; }
    if (bins[1].id || bins[2].id) { ext->bin_attr = bins[1].id ? &bins[1] : &bins[2]; ext->bin_attrs = (uint64_t)(!!bins[1].id + !!bins[2].id); }
    response->range.total = response->range.results = 1;
    response->rooms = ext;
    return response;
}

typedef struct {
    uint16_t ctx; uint32_t request; RequestCallback cb; void *arg; NpId me;
    M2IntSearchFilter filters[8]; uint64_t filter_count; JoinMessage join;
} LinkJob;

/* A joiner's room search: asks the host until its room is there (up to a minute), then answers. */
static void *search_main(void *arg) {
    LinkJob *job = arg;
    runtime_thread_attach_host("NpLinkSearch");
    M2SearchRoomResponse *response = NULL;
    pthread_mutex_lock(&m2_lock);
    for (int attempt = 0; attempt < 60 && !response; ++attempt) {
        Peer *host = host_link(&job->me);
        if (host && !link_send(host, MSG_QUERY, NULL, 0)) {
            Room *r = await_reply(MSG_ROOM, MSG_ROOM, 3000, NULL);
            if (r && room_matches(r, job->filters, job->filter_count)) {
                printf("Online: found the host's room %#llx\n", (unsigned long long)r->room_id);
                response = search_response(r);
            }
            free(r);
        }
        if (!response) { pthread_mutex_unlock(&m2_lock); sleep_ms(1000); pthread_mutex_lock(&m2_lock); }
    }
    pthread_mutex_unlock(&m2_lock);
    if (!response) {
        printf("Online: no room from the host; this player creates one\n");
        response = search_response(NULL);
    }
    if (job->cb) post((Event){.kind = EVENT_REQUEST, .ctx = job->ctx, .request = job->request, .event = M2_REQUEST_SEARCH_ROOM,
                              .data = response, .callback = (void *)job->cb, .arg = job->arg});
    else free(response);
    free(job);
    return NULL;
}

/* A joiner's JoinRoom: the host adds this player and answers with its room. */
static void *join_main(void *arg) {
    LinkJob *job = arg;
    runtime_thread_attach_host("NpLinkJoin");
    void *response = NULL;
    int32_t error = M2_ROOM_NOT_FOUND;
    pthread_mutex_lock(&m2_lock);
    Peer *host = host_link(&job->me);
    if (host && !link_send(host, MSG_JOIN, &job->join, sizeof(job->join))) {
        uint16_t got = 0;
        JoinedMessage *reply = await_reply(MSG_JOINED, MSG_REFUSED, 10000, &got);
        if (reply && got == MSG_JOINED) {
            room = reply->room;
            room_context = job->ctx;
            my_member = reply->member_id;
            response = room_snapshot(&room, my_member);
            error = response ? 0 : M2_INVALID_ARGUMENT;
            printf("Online: joined the room %#llx as member %u\n", (unsigned long long)room.room_id, my_member);
            for (int i = 0; i < M2_MAX_MEMBERS; ++i)
                if (room.members[i].used && room.members[i].member_id != my_member)
                    post_signaling(room.members[i].member_id, M2_SIGNALING_EVENT_ESTABLISHED);
        } else if (reply) {
            error = reply->error;
        }
        free(reply);
    }
    pthread_mutex_unlock(&m2_lock);
    if (job->cb) post((Event){.kind = EVENT_REQUEST, .ctx = job->ctx, .request = job->request, .event = M2_REQUEST_JOIN_ROOM,
                              .error = error, .data = response, .callback = (void *)job->cb, .arg = job->arg});
    else free(response);
    free(job);
    return NULL;
}

static LinkJob *new_job(M2Context *c, const M2RequestOptParam *opt, uint32_t *request_id) {
    LinkJob *job = calloc(1, sizeof(*job));
    if (!job) return NULL;
    job->ctx = c->id;
    job->cb = opt ? opt->callback : c->defaults.callback;
    job->arg = opt ? opt->arg : c->defaults.arg;
    job->me = c->np_id;
    pthread_mutex_lock(&m2_lock);
    job->request = next_request++;
    pthread_mutex_unlock(&m2_lock);
    if (request_id) *request_id = job->request;
    return job;
}

static void run_job(void *(*main)(void *), LinkJob *job) {
    pthread_t thread;
    pthread_create(&thread, NULL, main, job);
    pthread_detach(thread);
}

/* ---- the calls ---- */
static ABI int32_t m2_initialize(const void *param) {
    (void)param;
    pthread_mutex_lock(&m2_lock);
    int was = m2_initialized;
    m2_initialized = 1;
    int start = !event_thread_started;
    event_thread_started = 1;
    pthread_mutex_unlock(&m2_lock);
    if (was) return M2_ALREADY_INITIALIZED;
    if (start) {
        pthread_t thread;
        pthread_create(&thread, NULL, event_main, NULL);
        pthread_detach(thread);
    }
    return 0;
}

static ABI int32_t m2_terminate(void) {
    pthread_mutex_lock(&m2_lock);
    m2_initialized = 0;
    memset(contexts, 0, sizeof(contexts));
    pthread_mutex_unlock(&m2_lock);
    return 0;
}

static ABI int32_t m2_create_context(const M2CreateContextParameter *param, uint16_t *id) {
    if (!m2_initialized) return M2_NOT_INITIALIZED;
    if (!param || !id || !param->np_id) return M2_INVALID_ARGUMENT;
    for (size_t i = 0; i < sizeof(contexts) / sizeof(*contexts); ++i) {
        if (contexts[i].used) continue;
        memset(&contexts[i], 0, sizeof(contexts[i]));
        contexts[i].used = 1;
        contexts[i].id = (uint16_t)(i + 1);
        contexts[i].np_id = *param->np_id;
        *id = contexts[i].id;
        return 0;
    }
    return M2_CONTEXT_MAX;
}

static ABI int32_t m2_destroy_context(uint16_t id) {
    M2Context *c = find_context(id);
    if (!c) return M2_CONTEXT_NOT_FOUND;
    c->used = 0;
    return 0;
}

static ABI int32_t m2_register_context_callback(ContextCallback cb, void *arg) {
    if (!m2_initialized) return M2_NOT_INITIALIZED;
    context_cb = cb;
    context_arg = arg;
    return 0;
}

static ABI int32_t m2_register_room_event_callback(uint16_t id, RoomEventCallback cb, void *arg) {
    M2Context *c = find_context(id);
    if (!c) return M2_CONTEXT_NOT_FOUND;
    c->room_cb = cb;
    c->room_arg = arg;
    return 0;
}

static ABI int32_t m2_register_signaling_callback(uint16_t id, SignalingCallback cb, void *arg) {
    M2Context *c = find_context(id);
    if (!c) return M2_CONTEXT_NOT_FOUND;
    c->signaling_cb = cb;
    c->signaling_arg = arg;
    return 0;
}

static ABI int32_t m2_register_lobby_event_callback(uint16_t id, void *cb, void *arg) {
    (void)cb; (void)arg;
    return find_context(id) ? 0 : M2_CONTEXT_NOT_FOUND;
}

static ABI int32_t m2_set_default_request_opt(uint16_t id, const M2RequestOptParam *opt) {
    M2Context *c = find_context(id);
    if (!c) return M2_CONTEXT_NOT_FOUND;
    if (!opt) return M2_INVALID_ARGUMENT;
    c->defaults = *opt;
    return 0;
}

static ABI int32_t m2_context_start(uint16_t id, uint64_t timeout) {
    (void)timeout;
    M2Context *c = find_context(id);
    if (!c) return M2_CONTEXT_NOT_FOUND;
    c->started = 1;
    if (context_cb)
        post((Event){.kind = EVENT_CONTEXT, .ctx = id, .event = M2_CONTEXT_EVENT_STARTED, .cause = M2_CAUSE_CONTEXT_ACTION,
                     .callback = (void *)context_cb, .arg = context_arg});
    printf("Online: matching context %u started as %.16s (%s)\n", id, c->np_id.handle.data,
           join_address() ? "joins" : "hosts");
    return 0;
}

static ABI int32_t m2_context_stop(uint16_t id) {
    M2Context *c = find_context(id);
    if (!c) return M2_CONTEXT_NOT_FOUND;
    c->started = 0;
    if (context_cb)
        post((Event){.kind = EVENT_CONTEXT, .ctx = id, .event = M2_CONTEXT_EVENT_STOPPED, .cause = M2_CAUSE_CONTEXT_ACTION,
                     .callback = (void *)context_cb, .arg = context_arg});
    return 0;
}

static ABI int32_t m2_get_server_id(uint16_t id, uint16_t *server) {
    if (!find_context(id)) return M2_CONTEXT_NOT_FOUND;
    if (!server) return M2_INVALID_ARGUMENT;
    *server = M2_SERVER_ID;
    return 0;
}

static ABI int32_t m2_get_world_info_list(uint16_t id, const void *request, const M2RequestOptParam *opt, uint32_t *request_id) {
    M2Context *c = find_context(id);
    if (!c) return M2_CONTEXT_NOT_FOUND;
    if (!request) return M2_INVALID_ARGUMENT;
    M2WorldListResponse *response = calloc(1, sizeof(M2WorldListResponse) + sizeof(M2World));
    if (!response) return M2_INVALID_ARGUMENT;
    M2World *world = (M2World *)(response + 1);
    world->world_id = M2_WORLD_ID;
    world->rooms = room.active ? 1 : 0;
    world->room_members = (uint32_t)members_in(&room);
    response->world = world;
    response->count = 1;
    return answer(c, opt, request_id, M2_REQUEST_GET_WORLD_INFO_LIST, 0, response);
}

static ABI int32_t m2_search_room(uint16_t id, const M2SearchRoomRequest *request, const M2RequestOptParam *opt,
                                  uint32_t *request_id) {
    M2Context *c = find_context(id);
    if (!c) return M2_CONTEXT_NOT_FOUND;
    if (!request) return M2_INVALID_ARGUMENT;
    printf("Online: room search (world %u, %llu int filters, %llu bin filters)\n", request->world_id,
           (unsigned long long)request->int_filters, (unsigned long long)request->bin_filters);
    const M2IntSearchFilter *filters = request->int_filter;
    for (uint64_t i = 0; i < request->int_filters && filters; ++i)
        printf("Online:   filter %#x op %u value %u\n", filters[i].attr.id, filters[i].op, filters[i].attr.num);
    if (!join_address()) /* the host: nobody else's room to find, it creates its own */
        return answer(c, opt, request_id, M2_REQUEST_SEARCH_ROOM, 0, search_response(NULL));
    LinkJob *job = new_job(c, opt, request_id);
    if (!job) return M2_INVALID_ARGUMENT;
    for (uint64_t i = 0; i < request->int_filters && i < 8 && filters; ++i) job->filters[job->filter_count++] = filters[i];
    run_job(search_main, job);
    return 0;
}

static ABI int32_t m2_create_join_room(uint16_t id, const M2CreateJoinRoomRequest *request, const M2RequestOptParam *opt,
                                       uint32_t *request_id) {
    M2Context *c = find_context(id);
    if (!c) return M2_CONTEXT_NOT_FOUND;
    if (!request) return M2_INVALID_ARGUMENT;
    pthread_mutex_lock(&m2_lock);
    memset(&room, 0, sizeof(room));
    room.active = 1;
    room.room_id = ((uint64_t)time(NULL) << 16) | 0x4D54; /* "MT" */
    room.max_slot = request->max_slot ? request->max_slot : 2;
    room.flags = request->flags;
    room.owner = 1;
    room.next_member = 2;
    room_context = c->id;
    my_member = 1;
    RoomMember *me = &room.members[0];
    me->used = 1;
    me->member_id = 1;
    me->np_id = c->np_id;
    me->team = request->team_id;
    me->joined = ticks_now();
    for (uint64_t i = 0; i < request->internal_bins && i < 2; ++i) store_bin(&room.internal[i], &request->internal_bin[i], 1);
    for (uint64_t i = 0; i < request->external_bins && i < 2; ++i) store_bin(&room.external[i], &request->external_bin[i], 1);
    for (uint64_t i = 0; i < request->search_bins && i < 1; ++i) store_bin(&room.search_bin[i], &request->search_bin[i], 1);
    for (uint64_t i = 0; i < request->search_ints && i < 8; ++i) room.search_int[room.search_ints++] = request->search_int[i];
    for (uint64_t i = 0; i < request->member_bins && i < 2; ++i) store_bin(&me->bin[i], &request->member_bin[i], 1);
    M2JoinRoomResponse *response = room_snapshot(&room, 1);
    printf("Online: room %#llx created (%u slots, flags %#x)\n", (unsigned long long)room.room_id, room.max_slot, room.flags);
    for (int i = 0; i < room.search_ints; ++i)
        printf("Online:   searchable %#x = %u\n", room.search_int[i].id, room.search_int[i].num);
    pthread_mutex_unlock(&m2_lock);
    start_listener();
    return answer(c, opt, request_id, M2_REQUEST_CREATE_JOIN_ROOM, response ? 0 : M2_INVALID_ARGUMENT, response);
}

static ABI int32_t m2_join_room(uint16_t id, const M2JoinRoomRequest *request, const M2RequestOptParam *opt, uint32_t *request_id) {
    M2Context *c = find_context(id);
    if (!c) return M2_CONTEXT_NOT_FOUND;
    if (!request) return M2_INVALID_ARGUMENT;
    LinkJob *job = new_job(c, opt, request_id);
    if (!job) return M2_INVALID_ARGUMENT;
    job->join.room_id = request->room_id;
    job->join.team = request->team_id;
    job->join.flags = request->flags;
    for (uint64_t i = 0; i < request->member_bins && i < 2; ++i) store_bin(&job->join.bin[i], &request->member_bin[i], 0);
    printf("Online: joining the room %#llx\n", (unsigned long long)request->room_id);
    run_job(join_main, job);
    return 0;
}

static ABI int32_t m2_leave_room(uint16_t id, const M2LeaveRoomRequest *request, const M2RequestOptParam *opt, uint32_t *request_id) {
    M2Context *c = find_context(id);
    if (!c) return M2_CONTEXT_NOT_FOUND;
    if (!request) return M2_INVALID_ARGUMENT;
    uint64_t *response = malloc(sizeof(uint64_t));
    if (response) *response = request->room_id;
    pthread_mutex_lock(&m2_lock);
    if (room.room_id == request->room_id) {
        if (join_address()) { if (peers[0].used) link_send(&peers[0], MSG_LEAVE, NULL, 0); }
        else broadcast(MSG_DESTROYED, NULL, 0, NULL);
        memset(&room, 0, sizeof(room));
        my_member = 0;
    }
    pthread_mutex_unlock(&m2_lock);
    printf("Online: left room %#llx\n", (unsigned long long)request->room_id);
    return answer(c, opt, request_id, M2_REQUEST_LEAVE_ROOM, 0, response);
}

static ABI int32_t m2_set_room_data_external(uint16_t id, const M2SetRoomDataExternalRequest *request, const M2RequestOptParam *opt,
                                             uint32_t *request_id) {
    M2Context *c = find_context(id);
    if (!c) return M2_CONTEXT_NOT_FOUND;
    if (!request) return M2_INVALID_ARGUMENT;
    pthread_mutex_lock(&m2_lock);
    for (uint64_t i = 0; i < request->search_ints && i < 8; ++i) {
        int k = 0;
        while (k < room.search_ints && room.search_int[k].id != request->search_int[i].id) ++k;
        if (k == room.search_ints && room.search_ints < 8) ++room.search_ints;
        if (k < 8) room.search_int[k] = request->search_int[i];
    }
    for (uint64_t i = 0; i < request->bins && i < 2; ++i) store_bin(&room.external[i], &request->bin[i], room.owner);
    for (uint64_t i = 0; i < request->search_bins && i < 1; ++i) store_bin(&room.search_bin[i], &request->search_bin[i], room.owner);
    pthread_mutex_unlock(&m2_lock);
    return answer(c, opt, request_id, M2_REQUEST_SET_ROOM_DATA_EXTERNAL, 0, NULL);
}

static ABI int32_t m2_set_room_data_internal(uint16_t id, const M2SetRoomDataInternalRequest *request, const M2RequestOptParam *opt,
                                             uint32_t *request_id) {
    M2Context *c = find_context(id);
    if (!c) return M2_CONTEXT_NOT_FOUND;
    if (!request) return M2_INVALID_ARGUMENT;
    pthread_mutex_lock(&m2_lock);
    uint32_t previous = room.flags;
    room.flags = (room.flags & ~request->flag_filter) | (request->flag_attr & request->flag_filter);
    for (uint64_t i = 0; i < request->bins && i < 2; ++i) {
        int k = request->bin[i].id == 0x58 ? 1 : 0;
        store_bin(&room.internal[k], &request->bin[i], my_member);
    }
    if (!join_address()) send_update(M2_ROOM_EVENT_UPDATED_ROOM_DATA_INTERNAL, my_member, previous, 0, NULL);
    pthread_mutex_unlock(&m2_lock);
    return answer(c, opt, request_id, M2_REQUEST_SET_ROOM_DATA_INTERNAL, 0, NULL);
}

static ABI int32_t m2_set_room_member_data_internal(uint16_t id, const M2SetRoomMemberDataInternalRequest *request,
                                                    const M2RequestOptParam *opt, uint32_t *request_id) {
    M2Context *c = find_context(id);
    if (!c) return M2_CONTEXT_NOT_FOUND;
    if (!request) return M2_INVALID_ARGUMENT;
    pthread_mutex_lock(&m2_lock);
    RoomMember *m = find_member(&room, request->member_id ? request->member_id : my_member);
    if (m) {
        uint32_t previous = m->flags;
        if (request->team_id) m->team = request->team_id;
        m->flags = (m->flags & ~request->flag_filter) | (request->flag_attr & request->flag_filter);
        for (uint64_t k = 0; k < request->bins && k < 2; ++k) store_bin(&m->bin[k], &request->bin[k], m->member_id);
        if (join_address()) {
            MemberSetMessage s;
            memset(&s, 0, sizeof(s));
            s.member_id = m->member_id;
            s.team = request->team_id;
            s.flag_filter = request->flag_filter;
            s.flag_attr = request->flag_attr;
            s.bins = (uint32_t)(request->bins > 2 ? 2 : request->bins);
            memcpy(s.bin, m->bin, sizeof(s.bin));
            if (peers[0].used) link_send(&peers[0], MSG_MEMBER_SET, &s, sizeof(s));
        } else {
            send_update(M2_ROOM_EVENT_UPDATED_ROOM_MEMBER_DATA_INTERNAL, m->member_id, previous, 0, NULL);
        }
    }
    pthread_mutex_unlock(&m2_lock);
    return answer(c, opt, request_id, M2_REQUEST_SET_ROOM_MEMBER_DATA_INTERNAL, 0, NULL);
}

/* ---- Matching2 signaling: the other members are reachable as soon as they are in the room ---- */
typedef struct { uint32_t address; uint16_t port; uint8_t pad[2]; } M2SignalingAddress;

static uint32_t member_address(uint16_t member) {
    if (join_address()) return peers[0].address; /* every other member is behind the host */
    for (int i = 0; i < MAX_PEERS; ++i)
        if (peers[i].used && peers[i].member_id == member) return peers[i].address;
    return htonl(INADDR_LOOPBACK);
}

static ABI int32_t m2_signaling_status(uint16_t id, uint64_t room_id, uint16_t member, int32_t *status, uint32_t *address,
                                       uint16_t *port) {
    (void)room_id;
    if (!find_context(id)) return M2_CONTEXT_NOT_FOUND;
    pthread_mutex_lock(&m2_lock);
    int present = find_member(&room, member) && member != my_member;
    if (status) *status = present ? 2 : 0; /* ACTIVE, or INACTIVE */
    if (address) *address = present ? member_address(member) : 0;
    if (port) *port = htons((uint16_t)(link_port() + 1));
    pthread_mutex_unlock(&m2_lock);
    return 0;
}

static ABI int32_t m2_signaling_info(uint16_t id, uint64_t room_id, uint16_t member, int32_t code, void *info) {
    (void)room_id;
    if (!find_context(id)) return M2_CONTEXT_NOT_FOUND;
    if (!info) return M2_INVALID_ARGUMENT;
    pthread_mutex_lock(&m2_lock);
    RoomMember *m = find_member(&room, member);
    memset(info, 0, sizeof(NpId));
    switch (code) {
    case 1: *(uint32_t *)info = 10000; break;       /* RTT (µs) */
    case 2: *(uint32_t *)info = 10000000; break;    /* bandwidth */
    case 3: case 7: if (m) memcpy(info, &m->np_id, sizeof(NpId)); break;
    case 4: case 5: ((M2SignalingAddress *)info)->address = member_address(member);
        ((M2SignalingAddress *)info)->port = htons((uint16_t)(link_port() + 1)); break;
    default: break;                                   /* packet loss 0 */
    }
    pthread_mutex_unlock(&m2_lock);
    return m ? 0 : M2_INVALID_ARGUMENT;
}

typedef struct { uint64_t size; uint32_t local, mapped; int32_t nat_status; uint32_t pad; } M2SignalingNetInfo;
static ABI int32_t m2_signaling_local_net_info(M2SignalingNetInfo *info) {
    if (!info) return M2_INVALID_ARGUMENT;
    info->local = info->mapped = htonl(INADDR_LOOPBACK);
    info->nat_status = 1;
    return 0;
}

/* ---- WebApi: PSN's web services (sessions), answered here ----------------------------------------
 * Every request is logged (API group, path, method, body). Creating a session answers a new
 * sessionId as PSN does; anything else answers 200 and an empty JSON object. */
#define WEBAPI_INVALID_ARGUMENT ((int32_t)0x80552902)
#define WEBAPI_REQUEST_NOT_FOUND ((int32_t)0x80552908)
typedef struct { uint64_t content_length; const char *content_type; uint8_t reserved[16]; } WebApiContentParameter;
typedef struct {
    int used; int64_t id; int method; char group[64], path[256], response[512]; size_t response_size, read;
} WebApiRequest;
static WebApiRequest web_requests[16];
static int64_t next_web_request = 1;
static int32_t next_web_context = 1;
static int session_count;

static ABI int32_t webapi_create_context(int32_t lib_context, const NpOnlineId *id) {
    (void)lib_context; (void)id;
    pthread_mutex_lock(&m2_lock);
    int32_t ctx = next_web_context++;
    pthread_mutex_unlock(&m2_lock);
    return ctx;
}

static ABI int32_t webapi_create_request(int32_t ctx, const char *group, const char *path, int32_t method,
                                         const WebApiContentParameter *content, int64_t *request_id) {
    (void)ctx;
    if (!group || !path || !request_id) return WEBAPI_INVALID_ARGUMENT;
    static const char *const methods[] = {"GET", "POST", "PUT", "DELETE", "PATCH"};
    printf("Online: web API %s %s %s (%s, %llu bytes)\n", method >= 0 && method < 5 ? methods[method] : "?", group, path,
           content && content->content_type ? content->content_type : "no body",
           content ? (unsigned long long)content->content_length : 0ull);
    pthread_mutex_lock(&m2_lock);
    WebApiRequest *r = NULL;
    for (size_t i = 0; i < sizeof(web_requests) / sizeof(*web_requests) && !r; ++i)
        if (!web_requests[i].used) r = &web_requests[i];
    if (!r) r = &web_requests[0]; /* the game leaked requests: reuse the oldest slot */
    memset(r, 0, sizeof(*r));
    r->used = 1;
    r->id = next_web_request++;
    r->method = method;
    snprintf(r->group, sizeof(r->group), "%s", group);
    snprintf(r->path, sizeof(r->path), "%s", path);
    *request_id = r->id;
    pthread_mutex_unlock(&m2_lock);
    return 0;
}

static WebApiRequest *find_web_request(int64_t id) {
    for (size_t i = 0; i < sizeof(web_requests) / sizeof(*web_requests); ++i)
        if (web_requests[i].used && web_requests[i].id == id) return &web_requests[i];
    return NULL;
}

/* The body, printable parts only (multipart session bodies carry a PNG). */
static void log_body(const void *data, uint64_t size) {
    char text[701];
    size_t n = 0;
    for (uint64_t i = 0; i < size && n < sizeof(text) - 1; ++i) {
        unsigned char c = ((const unsigned char *)data)[i];
        text[n++] = (c >= 32 && c < 127) || c == '\n' ? (char)c : '.';
    }
    text[n] = 0;
    printf("Online: web API body (%llu bytes): %s\n", (unsigned long long)size, text);
}

static ABI int32_t webapi_send_request(int64_t id, const void *data, uint64_t size) {
    pthread_mutex_lock(&m2_lock);
    WebApiRequest *r = find_web_request(id);
    if (!r) { pthread_mutex_unlock(&m2_lock); return WEBAPI_REQUEST_NOT_FOUND; }
    if (data && size) log_body(data, size);
    if (r->method == 1 && strstr(r->path, "/sessions") && !strstr(r->path, "/sessions/"))
        snprintf(r->response, sizeof(r->response), "{\"sessionId\":\"%08X-%04X-4D54-8000-%012llX\"}",
                 (unsigned)time(NULL), ++session_count, (unsigned long long)room.room_id);
    else
        snprintf(r->response, sizeof(r->response), "{}");
    r->response_size = strlen(r->response);
    printf("Online: web API answer: %s\n", r->response);
    pthread_mutex_unlock(&m2_lock);
    return 0;
}

static ABI int32_t webapi_send_request2(int64_t id, const void *data, uint64_t size, void *info) {
    if (info) memset(info, 0, 16); /* response information: no error */
    return webapi_send_request(id, data, size);
}

static ABI int32_t webapi_status(int64_t id, int32_t *status) {
    if (!find_web_request(id)) return WEBAPI_REQUEST_NOT_FOUND;
    if (status) *status = 200;
    return 0;
}

static ABI int32_t webapi_read(int64_t id, void *data, uint64_t size) {
    pthread_mutex_lock(&m2_lock);
    WebApiRequest *r = find_web_request(id);
    int32_t n = 0;
    if (r && data && size) {
        n = (int32_t)(r->response_size - r->read < size ? r->response_size - r->read : size);
        memcpy(data, r->response + r->read, (size_t)n);
        r->read += (size_t)n;
    }
    pthread_mutex_unlock(&m2_lock);
    return r ? n : WEBAPI_REQUEST_NOT_FOUND;
}

static ABI int32_t webapi_header_length(int64_t id, const char *field, uint64_t *length) {
    WebApiRequest *r = find_web_request(id);
    if (!r || !length) return WEBAPI_REQUEST_NOT_FOUND;
    *length = field && !strcmp(field, "Content-Length") ? 8 : field && !strcmp(field, "Content-Type") ? 16 : 0;
    return 0;
}

static ABI int32_t webapi_header(int64_t id, const char *field, char *value, uint64_t size) {
    WebApiRequest *r = find_web_request(id);
    if (!r || !value || !size) return WEBAPI_REQUEST_NOT_FOUND;
    if (field && !strcmp(field, "Content-Length")) snprintf(value, size, "%zu", r->response_size);
    else if (field && !strcmp(field, "Content-Type")) snprintf(value, size, "application/json");
    else value[0] = 0;
    return 0;
}

static ABI int32_t webapi_delete_request(int64_t id) {
    pthread_mutex_lock(&m2_lock);
    WebApiRequest *r = find_web_request(id);
    if (r) r->used = 0;
    pthread_mutex_unlock(&m2_lock);
    return r ? 0 : WEBAPI_REQUEST_NOT_FOUND;
}

static ABI int32_t webapi_ok(void) { return 0; }
static ABI int32_t webapi_new_id(void) {
    pthread_mutex_lock(&m2_lock);
    int32_t id = next_web_context++;
    pthread_mutex_unlock(&m2_lock);
    return id;
}

static const RuntimeExport exports[] = {
    {"sceNpWebApiCreateContext", webapi_create_context},
    {"sceNpWebApiCreateRequest", webapi_create_request},
    {"sceNpWebApiSendRequest", webapi_send_request},
    {"sceNpWebApiSendRequest2", webapi_send_request2},
    {"sceNpWebApiGetHttpStatusCode", webapi_status},
    {"sceNpWebApiReadData", webapi_read},
    {"sceNpWebApiGetHttpResponseHeaderValueLength", webapi_header_length},
    {"sceNpWebApiGetHttpResponseHeaderValue", webapi_header},
    {"sceNpWebApiDeleteRequest", webapi_delete_request},
    {"sceNpWebApiDeleteContext", webapi_ok},
    {"sceNpWebApiAbortRequest", webapi_ok},
    {"sceNpWebApiGetErrorCode", webapi_ok},
    {"sceNpWebApiCreatePushEventFilter", webapi_new_id},
    {"sceNpWebApiRegisterPushEventCallback", webapi_new_id},
    {"sceNpMatching2Initialize", m2_initialize},
    {"sceNpMatching2Terminate", m2_terminate},
    {"sceNpMatching2CreateContext", m2_create_context},
    {"sceNpMatching2DestroyContext", m2_destroy_context},
    {"sceNpMatching2RegisterContextCallback", m2_register_context_callback},
    {"sceNpMatching2RegisterRoomEventCallback", m2_register_room_event_callback},
    {"sceNpMatching2RegisterSignalingCallback", m2_register_signaling_callback},
    {"sceNpMatching2RegisterLobbyEventCallback", m2_register_lobby_event_callback},
    {"sceNpMatching2SetDefaultRequestOptParam", m2_set_default_request_opt},
    {"sceNpMatching2ContextStart", m2_context_start},
    {"sceNpMatching2ContextStop", m2_context_stop},
    {"sceNpMatching2GetServerId", m2_get_server_id},
    {"sceNpMatching2GetWorldInfoList", m2_get_world_info_list},
    {"sceNpMatching2SearchRoom", m2_search_room},
    {"sceNpMatching2CreateJoinRoom", m2_create_join_room},
    {"sceNpMatching2JoinRoom", m2_join_room},
    {"sceNpMatching2LeaveRoom", m2_leave_room},
    {"sceNpMatching2SetRoomDataExternal", m2_set_room_data_external},
    {"sceNpMatching2SetRoomDataInternal", m2_set_room_data_internal},
    {"sceNpMatching2SetRoomMemberDataInternal", m2_set_room_member_data_internal},
    {"sceNpMatching2SignalingGetConnectionStatus", m2_signaling_status},
    {"sceNpMatching2SignalingGetConnectionInfo", m2_signaling_info},
    {"sceNpMatching2SignalingGetLocalNetInfo", m2_signaling_local_net_info},
    {"sceNpGetState", np_get_state},
    {"sceNpGetOnlineId", np_get_online_id},
    {"sceNpGetNpId", np_get_np_id},
    {"sceNpCheckNpAvailability", np_check_availability},
    {"sceNpCheckPlus", np_check_plus},
    {"sceNpGetParentalControlInfo", np_parental_control},
    {"sceNpPollAsync", np_poll_async},
    {"sceNpGetGamePresenceStatus", np_game_presence},
    {"sceNetCtlGetState", netctl_get_state},
};

/* Asked before runtime_services.c, so these replace its offline answers when MT_ONLINE is on. */
uintptr_t runtime_np_resolve(const char *name) { return online() ? RUNTIME_LOOKUP(exports, name) : 0; }
