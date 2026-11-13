/* NpScore (leaderboards) through a shadNet server (MT_SHADNET_SERVER): GODZILLA's VS ranking and the
 * scores it records after a match. Without shadNet, runtime_services.c answers "signed out" as before.
 *
 * The calls block until the server answers, as the PS4's synchronous NpScore calls do. Score commands
 * carry the 12-byte NP communication ID before their proto (shadNet documentation/protocol.md); the
 * game's npbind.dat names one, NPWR08361_00 (MT_SHADNET_COMID overrides it). A board the server does not
 * know yet is created with default settings when a first score is recorded to it. */
#define _GNU_SOURCE
#include "runtime.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CMD_RECORD_SCORE = 31, CMD_GET_SCORE_RANGE = 34, CMD_GET_SCORE_FRIENDS = 35, CMD_GET_SCORE_NPID = 36,
       ERR_NOT_FOUND = 26, ERR_SCORE_NOT_BEST = 29, MAX_OBJECTS = 32 };
#define COMMUNITY_INVALID_ARGUMENT ((int32_t)0x80550704)
#define COMMUNITY_TOO_MANY_OBJECTS ((int32_t)0x80550706)
#define COMMUNITY_ABORTED ((int32_t)0x80550707)
#define COMMUNITY_BAD_RESPONSE ((int32_t)0x80550708)
#define COMMUNITY_HTTP_SERVER ((int32_t)0x8055070A)
#define COMMUNITY_INVALID_ID ((int32_t)0x8055070E)
#define SERVER_INTERNAL_ERROR ((int32_t)0x80550807)
#define SERVER_RANKING_NOT_FOUND ((int32_t)0x80550812)
#define SERVER_NOT_BEST_SCORE ((int32_t)0x80550815)
#define SERVER_BOARD_NOT_FOUND ((int32_t)0x80550817)

typedef struct { char data[16]; int8_t term; int8_t dummy[3]; } OnlineId;
typedef struct { OnlineId handle; uint8_t opt[8]; uint8_t reserved[8]; } NpId;
typedef struct { char utf8[64]; } ScoreComment;
typedef struct { uint64_t size; uint8_t data[189]; uint8_t pad[3]; } ScoreGameInfo;
typedef struct {
    NpId np_id; uint8_t reserved[49]; uint8_t pad0[3]; int32_t pc_id; uint32_t serial_rank, rank, highest_rank;
    int32_t has_game_data; uint8_t pad1[4]; int64_t score; uint64_t record_date;
} RankData;
typedef struct { int32_t has_data; uint8_t pad[4]; RankData rank; } PlayerRankData;
_Static_assert(sizeof(NpId) == 36 && sizeof(RankData) == 0x80 && sizeof(PlayerRankData) == 0x88 &&
               sizeof(ScoreGameInfo) == 200 && sizeof(ScoreComment) == 64, "NpScore layouts");

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int contexts[MAX_OBJECTS];                 /* in use */
static struct { int used, context, aborted; } requests[MAX_OBJECTS];

static const char *communication_id(void) {
    const char *env = getenv("MT_SHADNET_COMID");
    return env && strlen(env) == 12 ? env : "NPWR08361_00";
}

/* ---- contexts and requests -------------------------------------------------------------------- */
static ABI int32_t create_title_ctx(uint32_t service_label, const NpId *self) {
    (void)service_label; (void)self;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_OBJECTS; ++i) {
        if (contexts[i]) continue;
        contexts[i] = 1;
        pthread_mutex_unlock(&lock);
        return i + 1;
    }
    pthread_mutex_unlock(&lock);
    return COMMUNITY_TOO_MANY_OBJECTS;
}

static ABI int32_t delete_title_ctx(int32_t id) {
    pthread_mutex_lock(&lock);
    int ok = id >= 1 && id <= MAX_OBJECTS && contexts[id - 1];
    if (ok) {
        contexts[id - 1] = 0;
        for (int i = 0; i < MAX_OBJECTS; ++i) if (requests[i].used && requests[i].context == id) requests[i].used = 0;
    }
    pthread_mutex_unlock(&lock);
    return ok ? 0 : COMMUNITY_INVALID_ID;
}

static ABI int32_t create_request(int32_t context) {
    pthread_mutex_lock(&lock);
    if (context < 1 || context > MAX_OBJECTS || !contexts[context - 1]) { pthread_mutex_unlock(&lock); return COMMUNITY_INVALID_ID; }
    for (int i = 0; i < MAX_OBJECTS; ++i) {
        if (requests[i].used) continue;
        requests[i].used = 1;
        requests[i].context = context;
        requests[i].aborted = 0;
        pthread_mutex_unlock(&lock);
        return i + 1;
    }
    pthread_mutex_unlock(&lock);
    return COMMUNITY_TOO_MANY_OBJECTS;
}

static int request_state(int32_t id) { /* 1 usable, 0 aborted, -1 unknown */
    pthread_mutex_lock(&lock);
    int state = id >= 1 && id <= MAX_OBJECTS && requests[id - 1].used ? !requests[id - 1].aborted : -1;
    pthread_mutex_unlock(&lock);
    return state;
}

static ABI int32_t delete_request(int32_t id) {
    pthread_mutex_lock(&lock);
    int ok = id >= 1 && id <= MAX_OBJECTS && requests[id - 1].used;
    if (ok) requests[id - 1].used = 0;
    pthread_mutex_unlock(&lock);
    return ok ? 0 : COMMUNITY_INVALID_ID;
}

static ABI int32_t abort_request(int32_t id) {
    pthread_mutex_lock(&lock);
    int ok = id >= 1 && id <= MAX_OBJECTS && requests[id - 1].used;
    if (ok) requests[id - 1].aborted = 1;
    pthread_mutex_unlock(&lock);
    return ok ? 0 : COMMUNITY_INVALID_ID;
}

/* ---- talking to the server --------------------------------------------------------------------- */
static int32_t error_from(uint8_t error) {
    switch (error) {
    case 0: return 0;
    case ERR_NOT_FOUND: return SERVER_BOARD_NOT_FOUND;
    case ERR_SCORE_NOT_BEST: return SERVER_NOT_BEST_SCORE;
    case 1: case 2: case 3: return COMMUNITY_BAD_RESPONSE;
    default: return SERVER_INTERNAL_ERROR;
    }
}

/* Sends a score command; 0 with the raw reply (after the error byte), or the PS4 error. */
static int32_t score_command(uint16_t command, const PbOut *proto, uint8_t **raw, uint32_t *raw_size) {
    uint8_t error = 0xFF;
    if (shadnet_request_raw(command, communication_id(), 12, proto, &error, raw, raw_size)) return COMMUNITY_HTTP_SERVER;
    if (error) {
        free(*raw);
        *raw = NULL;
        return error_from(error);
    }
    return 0;
}

/* The proto blob (u32 size, then bytes) at the start of a raw reply. */
static int reply_blob(const uint8_t *raw, uint32_t size, PbIn *in) {
    if (!raw || size < 4) return 0;
    uint32_t n = (uint32_t)raw[0] | (uint32_t)raw[1] << 8 | (uint32_t)raw[2] << 16 | (uint32_t)raw[3] << 24;
    if (n > size - 4) return 0;
    in->at = raw + 4;
    in->end = raw + 4 + n;
    return 1;
}

/* A GetScoreResponse into the game's arrays; returns the entries filled. `lookup`: for a query by players,
 * put each answer at the index of its player (ranks arrive in request order, unranked players empty). */
static int32_t fill_ranks(const uint8_t *raw, uint32_t size, RankData *plain, PlayerRankData *players, uint64_t slots,
                          ScoreComment *comments, ScoreGameInfo *infos, uint64_t *last_sort, uint32_t *total) {
    PbIn in;
    if (!reply_blob(raw, size, &in)) return COMMUNITY_BAD_RESPONSE;
    unsigned field;
    uint64_t value;
    const uint8_t *data;
    size_t length;
    uint64_t index = 0, comment_index = 0, info_index = 0;
    int32_t found = 0;
    while (pb_next(&in, &field, &value, &data, &length)) {
        if (field == 1 && index < slots) {
            RankData r;
            memset(&r, 0, sizeof(r));
            PbIn e = {data, data + length};
            unsigned f;
            uint64_t v;
            const uint8_t *b;
            size_t n;
            uint32_t shared = 0;
            while (pb_next(&e, &f, &v, &b, &n)) {
                switch (f) {
                case 1: memcpy(r.np_id.handle.data, b, n < 16 ? n : 16); break;
                case 2: r.pc_id = (int32_t)v; break;
                case 3: r.serial_rank = (uint32_t)v; break;
                case 4: r.score = (int64_t)v; break;
                case 5: r.has_game_data = v != 0; break;
                case 6: r.record_date = v; break;
                case 8: shared = (uint32_t)v; break;
                }
            }
            r.rank = r.highest_rank = shared ? shared : r.serial_rank;
            int ranked = r.np_id.handle.data[0] != 0;
            if (players) {
                players[index].has_data = ranked;
                if (ranked) players[index].rank = r;
            } else if (ranked) {
                plain[found] = r;
            }
            if (ranked) ++found;
            if (players || ranked) ++index;
        } else if (field == 2 && comments && comment_index < slots) {
            memcpy(comments[comment_index++].utf8, data, length < 63 ? length : 63);
        } else if (field == 3 && infos && info_index < slots) {
            PbIn e = {data, data + length};
            unsigned f;
            uint64_t v;
            const uint8_t *b;
            size_t n;
            while (pb_next(&e, &f, &v, &b, &n)) {
                if (f != 1) continue;
                infos[info_index].size = n < sizeof(infos->data) ? n : sizeof(infos->data);
                memcpy(infos[info_index].data, b, (size_t)infos[info_index].size);
            }
            ++info_index;
        } else if (field == 4 && last_sort) {
            *last_sort = value;
        } else if (field == 5 && total) {
            *total = (uint32_t)value;
        }
    }
    return found;
}

/* ---- the calls ---------------------------------------------------------------------------------- */
static ABI int32_t record_score(int32_t request, uint32_t board, int64_t score, const ScoreComment *comment,
                                const ScoreGameInfo *info, uint32_t *temporary_rank, const uint64_t *compare_date,
                                void *option) {
    (void)compare_date; (void)option;
    int state = request_state(request);
    if (state < 0) return COMMUNITY_INVALID_ID;
    if (!state) return COMMUNITY_ABORTED;
    if (info && info->size > sizeof(info->data)) return COMMUNITY_INVALID_ARGUMENT;
    PbOut out = {0};
    pb_uint(&out, 1, board);
    pb_uint(&out, 3, (uint64_t)score);
    if (comment) pb_bytes(&out, 4, comment->utf8, strnlen(comment->utf8, 63));
    if (info && info->size) pb_bytes(&out, 5, info->data, (size_t)info->size);
    uint8_t *raw = NULL;
    uint32_t size = 0;
    int32_t result = score_command(CMD_RECORD_SCORE, &out, &raw, &size);
    pb_free(&out);
    uint32_t rank = raw && size >= 4 ? (uint32_t)raw[0] | (uint32_t)raw[1] << 8 | (uint32_t)raw[2] << 16 | (uint32_t)raw[3] << 24 : 0;
    if (!result && temporary_rank) *temporary_rank = rank;
    free(raw);
    printf("Score: board %u, score %lld recorded: %s (rank %u)\n", board, (long long)score,
           result ? "refused" : "ok", rank);
    return result;
}

static int32_t check_arrays(uint64_t count, uint64_t rank_size, uint64_t entry, const void *comments, uint64_t comment_size,
                            const void *infos, uint64_t info_size) {
    if (!count || count > 101) return COMMUNITY_INVALID_ARGUMENT;
    if (rank_size != count * entry) return COMMUNITY_INVALID_ARGUMENT;
    if (comments && comment_size != count * sizeof(ScoreComment)) return COMMUNITY_INVALID_ARGUMENT;
    if (infos && info_size != count * sizeof(ScoreGameInfo)) return COMMUNITY_INVALID_ARGUMENT;
    return 0;
}

static int32_t finish_query(const char *what, uint32_t board, int32_t result, uint8_t *raw, uint32_t size, RankData *plain,
                            PlayerRankData *players, uint64_t count, ScoreComment *comments, ScoreGameInfo *infos,
                            uint64_t *last_sort, uint32_t *total) {
    if (!result) {
        result = fill_ranks(raw, size, plain, players, count, comments, infos, last_sort, total);
        if (result == 0) result = SERVER_RANKING_NOT_FOUND;
    }
    free(raw);
    printf("Score: %s on board %u: %d%s\n", what, board, result > 0 ? result : 0, result < 0 ? " (none or error)" : " entries");
    return result;
}

static ABI int32_t ranking_by_range(int32_t request, uint32_t board, uint32_t start, RankData *ranks, uint64_t ranks_size,
                                    ScoreComment *comments, uint64_t comments_size, ScoreGameInfo *infos,
                                    uint64_t infos_size, uint64_t count, uint64_t *last_sort, uint32_t *total, void *option) {
    (void)option;
    int state = request_state(request);
    if (state < 0) return COMMUNITY_INVALID_ID;
    if (!state) return COMMUNITY_ABORTED;
    int32_t bad = check_arrays(count, ranks_size, sizeof(RankData), comments, comments_size, infos, infos_size);
    if (bad || !ranks || !start) return bad ? bad : COMMUNITY_INVALID_ARGUMENT;
    memset(ranks, 0, ranks_size);
    if (comments) memset(comments, 0, comments_size);
    if (infos) memset(infos, 0, infos_size);
    if (last_sort) *last_sort = 0;
    if (total) *total = 0;
    PbOut out = {0};
    pb_uint(&out, 1, board);
    pb_uint(&out, 2, start);
    pb_uint(&out, 3, count);
    pb_bool(&out, 4, comments != NULL);
    pb_bool(&out, 5, infos != NULL);
    uint8_t *raw = NULL;
    uint32_t size = 0;
    int32_t result = score_command(CMD_GET_SCORE_RANGE, &out, &raw, &size);
    pb_free(&out);
    return finish_query("ranking by range", board, result, raw, size, ranks, NULL, count, comments, infos, last_sort, total);
}

static ABI int32_t friends_ranking(int32_t request, uint32_t board, int32_t include_self, RankData *ranks,
                                   uint64_t ranks_size, ScoreComment *comments, uint64_t comments_size,
                                   ScoreGameInfo *infos, uint64_t infos_size, uint64_t count, uint64_t *last_sort,
                                   uint32_t *total, void *option) {
    (void)option;
    int state = request_state(request);
    if (state < 0) return COMMUNITY_INVALID_ID;
    if (!state) return COMMUNITY_ABORTED;
    int32_t bad = check_arrays(count, ranks_size, sizeof(RankData), comments, comments_size, infos, infos_size);
    if (bad || !ranks) return bad ? bad : COMMUNITY_INVALID_ARGUMENT;
    memset(ranks, 0, ranks_size);
    if (comments) memset(comments, 0, comments_size);
    if (infos) memset(infos, 0, infos_size);
    if (last_sort) *last_sort = 0;
    if (total) *total = 0;
    PbOut out = {0};
    pb_uint(&out, 1, board);
    pb_bool(&out, 2, include_self);
    pb_uint(&out, 3, count);
    pb_bool(&out, 4, comments != NULL);
    pb_bool(&out, 5, infos != NULL);
    uint8_t *raw = NULL;
    uint32_t size = 0;
    int32_t result = score_command(CMD_GET_SCORE_FRIENDS, &out, &raw, &size);
    pb_free(&out);
    return finish_query("friends ranking", board, result, raw, size, ranks, NULL, count, comments, infos, last_sort, total);
}

static ABI int32_t ranking_by_np_id(int32_t request, uint32_t board, const NpId *ids, uint64_t ids_size,
                                    PlayerRankData *ranks, uint64_t ranks_size, ScoreComment *comments,
                                    uint64_t comments_size, ScoreGameInfo *infos, uint64_t infos_size, uint64_t count,
                                    uint64_t *last_sort, uint32_t *total, void *option) {
    (void)option;
    int state = request_state(request);
    if (state < 0) return COMMUNITY_INVALID_ID;
    if (!state) return COMMUNITY_ABORTED;
    int32_t bad = check_arrays(count, ranks_size, sizeof(PlayerRankData), comments, comments_size, infos, infos_size);
    if (bad || !ranks || !ids || ids_size != count * sizeof(NpId)) return bad ? bad : COMMUNITY_INVALID_ARGUMENT;
    memset(ranks, 0, ranks_size);
    if (comments) memset(comments, 0, comments_size);
    if (infos) memset(infos, 0, infos_size);
    if (last_sort) *last_sort = 0;
    if (total) *total = 0;
    PbOut out = {0};
    pb_uint(&out, 1, board);
    for (uint64_t i = 0; i < count; ++i) {
        PbOut id = {0};
        pb_bytes(&id, 1, ids[i].handle.data, strnlen(ids[i].handle.data, 16));
        pb_msg(&out, 2, &id);
        pb_free(&id);
    }
    pb_bool(&out, 3, comments != NULL);
    pb_bool(&out, 4, infos != NULL);
    uint8_t *raw = NULL;
    uint32_t size = 0;
    int32_t result = score_command(CMD_GET_SCORE_NPID, &out, &raw, &size);
    pb_free(&out);
    return finish_query("ranking by player", board, result, raw, size, NULL, ranks, count, comments, infos, last_sort, total);
}

static ABI int32_t score_ok(void) { return 0; }

static const RuntimeExport exports[] = {
    {"sceNpScoreCreateNpTitleCtx", create_title_ctx},
    {"sceNpScoreDeleteNpTitleCtx", delete_title_ctx},
    {"sceNpScoreCreateRequest", create_request},
    {"sceNpScoreDeleteRequest", delete_request},
    {"sceNpScoreAbortRequest", abort_request},
    {"sceNpScoreRecordScore", record_score},
    {"sceNpScoreGetRankingByRange", ranking_by_range},
    {"sceNpScoreGetFriendsRanking", friends_ranking},
    {"sceNpScoreGetRankingByNpId", ranking_by_np_id},
    {"sceNpScoreSetPlayerCharacterId", score_ok},
};

uintptr_t runtime_score_resolve(const char *name) { return shadnet_enabled() ? RUNTIME_LOOKUP(exports, name) : 0; }
