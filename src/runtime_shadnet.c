/* shadNet client: the community server that stands in for PSN (https://github.com/shadps4-emu/shadNet,
 * GPL-2.0-or-later). One TCP connection carries every command; a reader thread matches replies to the
 * waiting request by packet id and hands server notifications (room events) to runtime_np.c.
 *
 * MT_SHADNET_SERVER   host[:port] of the server (port 31313 by default). Unset: shadNet is not used.
 * MT_SHADNET_USER     the account name (3-16 letters, digits, - and _), also the player's Online ID.
 * MT_SHADNET_PASSWORD its password.
 * MT_SHADNET_EMAIL    with it, a first sign-in to an account that does not exist yet registers it.
 * MT_SHADNET_SECRET   the server's registration key, when its operator set one.
 * MT_SHADNET_UDP_PORT the server's UDP signaling port (31314 by default).
 *
 * Packets (shadNet documentation/protocol.md): a 15-byte header (u8 type, u16 command, u32 total size,
 * u64 packet id, little-endian), then the payload. Requests carry a proto blob (u32 size + bytes);
 * replies an error byte, then the reply's proto blob when there is one. */
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
typedef SOCKET NetSocket;
#define close_socket closesocket
#define SEND_FLAGS 0
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int NetSocket;
#define INVALID_SOCKET (-1)
#define close_socket close
#define SEND_FLAGS MSG_NOSIGNAL
#endif

#ifndef MT_VERSION_TEXT
#define MT_VERSION_TEXT "MinusTwo GODZILLA"
#endif

enum {
    HEADER_SIZE = 15, MAX_PACKET = 0x800000, DEFAULT_PORT = 31313, DEFAULT_UDP_PORT = 31314,
    TYPE_REQUEST = 0, TYPE_REPLY = 1, TYPE_NOTIFICATION = 2, TYPE_SERVER_INFO = 3,
    CMD_LOGIN = 0, CMD_CREATE = 2, CMD_GET_SERVER_FEATURES = 12, CMD_SET_CLIENT_VERSION = 13,
    KEEPALIVE_SECONDS = 20, /* idle connections are dropped by the server and by routers */
    ERR_LOGIN_INVALID_PASSWORD = 8, ERR_CREATION_EXISTING_USERNAME = 11,
    REPLY_TIMEOUT_MS = 15000,
};

/* ---- protobuf ---------------------------------------------------------------------------------- */
static void pb_put(PbOut *out, const void *data, size_t size) {
    if (out->failed) return;
    if (out->size + size > out->capacity) {
        size_t capacity = out->capacity ? out->capacity : 64;
        while (capacity < out->size + size) capacity *= 2;
        uint8_t *grown = realloc(out->data, capacity);
        if (!grown) { out->failed = 1; return; }
        out->data = grown;
        out->capacity = capacity;
    }
    if (size) memcpy(out->data + out->size, data, size);
    out->size += size;
}

static void pb_varint(PbOut *out, uint64_t value) {
    uint8_t bytes[10];
    size_t n = 0;
    do {
        bytes[n] = (uint8_t)(value & 0x7F);
        value >>= 7;
        if (value) bytes[n] |= 0x80;
        ++n;
    } while (value);
    pb_put(out, bytes, n);
}

void pb_uint(PbOut *out, unsigned field, uint64_t value) {
    if (!value) return;
    pb_varint(out, (uint64_t)field << 3);
    pb_varint(out, value);
}

void pb_bool(PbOut *out, unsigned field, int value) { pb_uint(out, field, value ? 1 : 0); }

void pb_bytes(PbOut *out, unsigned field, const void *data, size_t size) {
    if (!size) return;
    pb_varint(out, (uint64_t)field << 3 | 2);
    pb_varint(out, size);
    pb_put(out, data, size);
}

void pb_str(PbOut *out, unsigned field, const char *text) { if (text) pb_bytes(out, field, text, strlen(text)); }

void pb_msg(PbOut *out, unsigned field, const PbOut *inner) {
    pb_varint(out, (uint64_t)field << 3 | 2);
    pb_varint(out, inner->size);
    pb_put(out, inner->data, inner->size);
    if (inner->failed) out->failed = 1;
}

void pb_free(PbOut *out) {
    free(out->data);
    memset(out, 0, sizeof(*out));
}

static int pb_read_varint(PbIn *in, uint64_t *value) {
    uint64_t result = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (in->at >= in->end) return 0;
        uint8_t b = *in->at++;
        result |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) { *value = result; return 1; }
    }
    return 0;
}

int pb_next(PbIn *in, unsigned *field, uint64_t *value, const uint8_t **data, size_t *size) {
    uint64_t key;
    *value = 0;
    *data = NULL;
    *size = 0;
    if (in->at >= in->end || !pb_read_varint(in, &key)) return 0;
    *field = (unsigned)(key >> 3);
    switch (key & 7) {
    case 0: return pb_read_varint(in, value);
    case 1:
        if (in->end - in->at < 8) return 0;
        memcpy(value, in->at, 8);
        in->at += 8;
        return 1;
    case 5: {
        if (in->end - in->at < 4) return 0;
        uint32_t v;
        memcpy(&v, in->at, 4);
        *value = v;
        in->at += 4;
        return 1;
    }
    case 2: {
        uint64_t n;
        if (!pb_read_varint(in, &n) || n > (uint64_t)(in->end - in->at)) return 0;
        *data = in->at;
        *size = (size_t)n;
        in->at += n;
        return 1;
    }
    default: return 0;
    }
}

/* ---- the connection ------------------------------------------------------------------------------ */
typedef struct Waiter {
    struct Waiter *next; uint64_t packet_id; int done; uint8_t error; uint8_t *reply; uint32_t reply_size;
    uint8_t *raw; uint32_t raw_size; /* everything after the error byte */
} Waiter;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;   /* connection state, waiters */
static pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t sign_in_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t replied = PTHREAD_COND_INITIALIZER;
static NetSocket connection = INVALID_SOCKET;
static int signed_in;
static uint64_t next_packet = 1;
static Waiter *waiters;
static ShadnetNotify notify_cb;
static char npid[17];
static uint64_t user_id;
static uint32_t server_ip;

/* MT_ONLINE_DEBUG=1: the online code's detailed log (room and member data, events handed to the game,
 * the first Rudp and P2P traffic). */
int runtime_online_debug(void) {
    static int value = -1;
    if (value < 0) {
        const char *env = getenv("MT_ONLINE_DEBUG");
        value = env && *env == '1';
    }
    return value;
}

int shadnet_enabled(void) {
    const char *server = getenv("MT_SHADNET_SERVER");
    return server && *server;
}

const char *shadnet_npid(void) {
    if (!npid[0]) {
        const char *user = getenv("MT_SHADNET_USER");
        snprintf(npid, sizeof(npid), "%s", user && *user ? user : "MinusTwo");
    }
    return npid;
}

uint64_t shadnet_user_id(void) { return user_id; }
uint32_t shadnet_server_ip(void) { return server_ip; }

uint16_t shadnet_signaling_port(void) {
    const char *port = getenv("MT_SHADNET_UDP_PORT");
    int value = port ? atoi(port) : 0;
    return (uint16_t)(value > 0 && value < 65536 ? value : DEFAULT_UDP_PORT);
}

void shadnet_set_notify(ShadnetNotify notify) { notify_cb = notify; }

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

static int send_all(NetSocket s, const void *data, size_t size) {
    const char *p = data;
    while (size) {
        int n = send(s, p, (int)size, SEND_FLAGS);
        if (n <= 0) return -1;
        p += n;
        size -= (size_t)n;
    }
    return 0;
}

static int recv_all(NetSocket s, void *data, size_t size) {
    char *p = data;
    while (size) {
        int n = recv(s, p, (int)size, 0);
        if (n <= 0) return -1;
        p += n;
        size -= (size_t)n;
    }
    return 0;
}

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static void put_u64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get_u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get_u64(const uint8_t *p) { return (uint64_t)get_u32(p) | (uint64_t)get_u32(p + 4) << 32; }

/* The connection is gone (lock held): every waiting request ends without an answer. */
static void connection_lost(void) {
    if (connection != INVALID_SOCKET) {
        close_socket(connection);
        connection = INVALID_SOCKET;
        printf("shadNet: the connection to the server closed\n");
    }
    signed_in = 0;
    for (Waiter *w = waiters; w; w = w->next) w->done = -1;
    pthread_cond_broadcast(&replied);
}

static void *reader_main(void *arg) {
    NetSocket s = (NetSocket)(uintptr_t)arg;
    runtime_thread_attach_host("shadNet");
    uint8_t header[HEADER_SIZE];
    while (!recv_all(s, header, sizeof(header))) {
        uint8_t type = header[0];
        uint16_t command = get_u16(header + 1);
        uint32_t size = get_u32(header + 3);
        uint64_t packet_id = get_u64(header + 7);
        if (size < HEADER_SIZE || size > MAX_PACKET) break;
        uint32_t payload_size = size - HEADER_SIZE;
        uint8_t *payload = payload_size ? malloc(payload_size) : NULL;
        if (payload_size && (!payload || recv_all(s, payload, payload_size))) { free(payload); break; }
        if (type == TYPE_REPLY) {
            pthread_mutex_lock(&lock);
            Waiter *w = waiters;
            while (w && w->packet_id != packet_id) w = w->next;
            if (w) {
                w->error = payload_size ? payload[0] : 0xFF;
                if (payload_size > 1 && (w->raw = malloc(payload_size - 1)) != NULL) {
                    memcpy(w->raw, payload + 1, payload_size - 1);
                    w->raw_size = payload_size - 1;
                }
                if (payload_size >= 5) {
                    uint32_t blob = get_u32(payload + 1);
                    if (blob <= payload_size - 5 && blob) {
                        w->reply = malloc(blob);
                        if (w->reply) { memcpy(w->reply, payload + 5, blob); w->reply_size = blob; }
                    }
                }
                w->done = 1;
                pthread_cond_broadcast(&replied);
            }
            pthread_mutex_unlock(&lock);
        } else if (type == TYPE_NOTIFICATION) {
            /* Room and friend notifications are proto blobs: a u32 size, then the message. */
            const uint8_t *message = payload;
            uint32_t message_size = payload_size;
            if (payload_size >= 4 && get_u32(payload) == payload_size - 4) { message += 4; message_size -= 4; }
            if (notify_cb) notify_cb(command, message, message_size);
        }
        free(payload);
    }
    pthread_mutex_lock(&lock);
    if (connection == s) connection_lost();
    pthread_mutex_unlock(&lock);
    return NULL;
}

/* Sends a request and waits for its reply. */
/* A command: `prefix` (raw bytes, may be NULL) then the proto blob; the reply's proto blob, or with `raw`
 * everything after its error byte. */
static int request_ex(uint16_t command, const void *prefix, uint32_t prefix_size, const PbOut *proto, uint8_t *error,
                      uint8_t **reply, uint32_t *reply_size, uint8_t **raw, uint32_t *raw_size) {
    if (raw) *raw = NULL;
    if (raw_size) *raw_size = 0;
    if (error) *error = 0xFF;
    if (reply) *reply = NULL;
    if (reply_size) *reply_size = 0;
    if (proto && proto->failed) return -1;
    uint32_t blob = proto ? (uint32_t)proto->size : 0;
    uint32_t size = HEADER_SIZE + prefix_size + 4 + blob;
    uint8_t *packet = malloc(size);
    if (!packet) return -1;
    Waiter w;
    memset(&w, 0, sizeof(w));
    pthread_mutex_lock(&lock);
    NetSocket s = connection;
    if (s == INVALID_SOCKET) { pthread_mutex_unlock(&lock); free(packet); return -1; }
    w.packet_id = next_packet++;
    w.next = waiters;
    waiters = &w;
    pthread_mutex_unlock(&lock);
    packet[0] = TYPE_REQUEST;
    put_u16(packet + 1, command);
    put_u32(packet + 3, size);
    put_u64(packet + 7, w.packet_id);
    if (prefix_size) memcpy(packet + HEADER_SIZE, prefix, prefix_size);
    put_u32(packet + HEADER_SIZE + prefix_size, blob);
    if (blob) memcpy(packet + HEADER_SIZE + prefix_size + 4, proto->data, blob);
    pthread_mutex_lock(&send_lock);
    int failed = send_all(s, packet, size);
    pthread_mutex_unlock(&send_lock);
    free(packet);
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += REPLY_TIMEOUT_MS / 1000;
    pthread_mutex_lock(&lock);
    while (!failed && !w.done)
        if (pthread_cond_timedwait(&replied, &lock, &until)) break;
    for (Waiter **p = &waiters; *p; p = &(*p)->next)
        if (*p == &w) { *p = w.next; break; }
    pthread_mutex_unlock(&lock);
    if (failed || w.done != 1) {
        free(w.reply);
        free(w.raw);
        printf("shadNet: no answer to command %u\n", command);
        return -1;
    }
    if (error) *error = w.error;
    if (reply && reply_size) { *reply = w.reply; *reply_size = w.reply_size; }
    else free(w.reply);
    if (raw && raw_size) { *raw = w.raw; *raw_size = w.raw_size; }
    else free(w.raw);
    return 0;
}

static int request(uint16_t command, const PbOut *proto, uint8_t *error, uint8_t **reply, uint32_t *reply_size) {
    return request_ex(command, NULL, 0, proto, error, reply, reply_size, NULL, NULL);
}

int shadnet_request_raw(uint16_t command, const void *prefix, uint32_t prefix_size, const PbOut *proto, uint8_t *error,
                        uint8_t **raw, uint32_t *raw_size) {
    if (shadnet_sign_in()) {
        if (error) *error = 0xFF;
        if (raw) *raw = NULL;
        if (raw_size) *raw_size = 0;
        return -1;
    }
    return request_ex(command, prefix, prefix_size, proto, error, NULL, NULL, raw, raw_size);
}

int shadnet_request(uint16_t command, const PbOut *proto, uint8_t *error, uint8_t **reply, uint32_t *reply_size) {
    if (shadnet_sign_in()) {
        if (error) *error = 0xFF;
        if (reply) *reply = NULL;
        if (reply_size) *reply_size = 0;
        return -1;
    }
    return request(command, proto, error, reply, reply_size);
}

/* Opens the TCP connection and reads the server's version greeting. */
static int open_connection(void) {
    const char *server = getenv("MT_SHADNET_SERVER");
    char host[256], port[16];
    snprintf(host, sizeof(host), "%s", server);
    char *colon = strrchr(host, ':');
    snprintf(port, sizeof(port), "%d", colon ? atoi(colon + 1) : DEFAULT_PORT);
    if (colon) *colon = 0;
    sockets_init();
    struct addrinfo hints, *found = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &found) || !found) {
        printf("shadNet: cannot find the server %s\n", host);
        return -1;
    }
    NetSocket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    uint32_t ip = ((struct sockaddr_in *)found->ai_addr)->sin_addr.s_addr;
    if (s != INVALID_SOCKET && connect(s, found->ai_addr, (socklen_t)found->ai_addrlen)) {
        close_socket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(found);
    if (s == INVALID_SOCKET) {
        printf("shadNet: cannot connect to %s:%s\n", host, port);
        return -1;
    }
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));
    uint8_t header[HEADER_SIZE];
    uint8_t version[4] = {0};
    if (recv_all(s, header, sizeof(header)) || header[0] != TYPE_SERVER_INFO || get_u32(header + 3) != HEADER_SIZE + 4 ||
        recv_all(s, version, sizeof(version))) {
        printf("shadNet: %s:%s did not answer as a shadNet server\n", host, port);
        close_socket(s);
        return -1;
    }
    printf("shadNet: connected to %s:%s (protocol %u)\n", host, port, get_u32(version));
    pthread_mutex_lock(&lock);
    connection = s;
    server_ip = ip;
    pthread_mutex_unlock(&lock);
    pthread_t thread;
    pthread_create(&thread, NULL, reader_main, (void *)(uintptr_t)s);
    pthread_detach(thread);
    return 0;
}

static int login(uint8_t *error) {
    const char *password = getenv("MT_SHADNET_PASSWORD");
    const char *title = getenv("MT_SHADNET_TITLE");
    PbOut out = {0};
    pb_str(&out, 1, shadnet_npid());
    pb_str(&out, 2, password ? password : "");
    pb_str(&out, 4, title && *title ? title : "CUSA02046");
    pb_str(&out, 5, "GODZILLA");
    uint8_t *reply = NULL;
    uint32_t reply_size = 0;
    int failed = request(CMD_LOGIN, &out, error, &reply, &reply_size);
    pb_free(&out);
    if (!failed && *error == 0) {
        PbIn in = {reply, reply + reply_size};
        unsigned field;
        uint64_t value;
        const uint8_t *data;
        size_t size;
        while (pb_next(&in, &field, &value, &data, &size))
            if (field == 2) user_id = value;
    }
    free(reply);
    return failed;
}

static int create_account(uint8_t *error) {
    const char *password = getenv("MT_SHADNET_PASSWORD");
    PbOut out = {0};
    pb_str(&out, 1, shadnet_npid());
    pb_str(&out, 2, password ? password : "");
    pb_str(&out, 4, getenv("MT_SHADNET_EMAIL"));
    pb_str(&out, 5, getenv("MT_SHADNET_SECRET"));
    int failed = request(CMD_CREATE, &out, error, NULL, NULL);
    pb_free(&out);
    return failed;
}

/* While signed in, a small request every KEEPALIVE_SECONDS keeps the connection from looking idle. */
static void *keepalive_main(void *unused) {
    (void)unused;
    runtime_thread_attach_host("shadNetKeepalive");
    for (;;) {
#ifdef _WIN32
        Sleep(KEEPALIVE_SECONDS * 1000);
#else
        sleep(KEEPALIVE_SECONDS);
#endif
        pthread_mutex_lock(&lock);
        int up = signed_in;
        pthread_mutex_unlock(&lock);
        if (up) request(CMD_GET_SERVER_FEATURES, NULL, NULL, NULL, NULL);
    }
    return NULL;
}

int shadnet_sign_in(void) {
    if (!shadnet_enabled()) return -1;
    pthread_mutex_lock(&sign_in_lock);
    pthread_mutex_lock(&lock);
    int already = signed_in;
    pthread_mutex_unlock(&lock);
    if (already) { pthread_mutex_unlock(&sign_in_lock); return 0; }
    int result = -1;
    if (open_connection()) goto done;
    uint8_t error = 0xFF;
    if (login(&error)) goto done;
    const char *email = getenv("MT_SHADNET_EMAIL");
    if (error == ERR_LOGIN_INVALID_PASSWORD && email && *email) {
        uint8_t created = 0xFF;
        if (create_account(&created)) goto done;
        if (created == 0) printf("shadNet: registered the account %s\n", shadnet_npid());
        else if (created != ERR_CREATION_EXISTING_USERNAME) printf("shadNet: registration refused (error %u)\n", created);
        if (login(&error)) goto done;
    }
    if (error) {
        printf("shadNet: sign-in as %s refused (error %u%s)\n", shadnet_npid(), error,
               error == ERR_LOGIN_INVALID_PASSWORD ? ": wrong name or password" : "");
        goto done;
    }
    PbOut version = {0};
    pb_str(&version, 1, MT_VERSION_TEXT);
    request(CMD_SET_CLIENT_VERSION, &version, &error, NULL, NULL);
    pb_free(&version);
    pthread_mutex_lock(&lock);
    signed_in = 1;
    static int keepalive_started;
    int start_keepalive = !keepalive_started;
    keepalive_started = 1;
    pthread_mutex_unlock(&lock);
    if (start_keepalive) {
        pthread_t thread;
        pthread_create(&thread, NULL, keepalive_main, NULL);
        pthread_detach(thread);
    }
    printf("shadNet: signed in as %s (account %llu)\n", shadnet_npid(), (unsigned long long)user_id);
    result = 0;
done:
    if (result) {
        pthread_mutex_lock(&lock);
        connection_lost();
        pthread_mutex_unlock(&lock);
    }
    pthread_mutex_unlock(&sign_in_lock);
    return result;
}
