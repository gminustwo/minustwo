/* The Discord status ("Rich Presence"): the player's Discord profile says they are playing, which mode
 * ("King of Kaiju Mode", "Main menu": runtime_state.c) and for how long. Built the way PrimeRecomp's is.
 *
 * Only the Discord app on this computer is talked to, through its local IPC endpoint: \\.\pipe\discord-ipc-N
 * on Windows, a Unix socket discord-ipc-N in $XDG_RUNTIME_DIR, $TMPDIR or /tmp on Linux and macOS (also where
 * the Flatpak and Snap Discords and Vesktop put theirs). Each frame is an opcode and a length (two
 * little-endian u32s), then JSON: a handshake with the application id, then SET_ACTIVITY commands; the app
 * answers PINGs. Nothing goes out to the network from here. Without the app running it looks again every
 * 20 s. The menu's Game tab (minustwo.ini discord_status) turns it off, and the status is cleared at once.
 * Discord takes 5 updates in 20 s: a change goes out at most every 5 s, the latest one winning.
 * Scripted test runs (MT_PAD_FILE, MT_WINDOW_BACKGROUND=1) leave the player's Discord alone unless they name
 * an application (MT_DISCORD_APP_ID); MT_DISCORD_PIPE=<name> talks to that endpoint instead (a stand-in
 * Discord: tools/fake_discord.py): \\.\pipe\<name> on Windows, $TMPDIR/<name> (or /tmp) elsewhere, or a
 * whole path. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/mtgpu.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

/* Minus Two's application on Discord (discord.com/developers/applications): its name is what the profile
 * says is being played, its Rich Presence art asset "icon" the picture. Empty: no Discord status. */
#define DISCORD_APP_ID "1573226184366497813"

enum { OP_HANDSHAKE = 0, OP_FRAME = 1, OP_CLOSE = 2, OP_PING = 3, OP_PONG = 4 };

/* MT_DISCORD_PIPE's endpoint: the name in the system's place for them, or the path as given. */
static int test_endpoint(char *path, size_t cap) {
    const char *name = getenv("MT_DISCORD_PIPE");
    if (!name || !*name) return 0;
    if (strchr(name, '/') || strchr(name, '\\')) snprintf(path, cap, "%s", name);
#ifdef _WIN32
    else snprintf(path, cap, "\\\\.\\pipe\\%s", name);
#else
    else snprintf(path, cap, "%s/%s", getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", name);
#endif
    return 1;
}

static void nap(int ms) {
    const struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

/* ---- the connection (one thread does all its I/O) ---- */
#ifdef _WIN32
typedef HANDLE Conn;
#define NO_CONN INVALID_HANDLE_VALUE

static Conn conn_try(const char *path) {
    return CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
}

static Conn conn_open(char *name, size_t cap) {
    char pipe[512];
    if (test_endpoint(pipe, sizeof pipe)) {
        Conn h = conn_try(pipe);
        if (h != NO_CONN) snprintf(name, cap, "MT_DISCORD_PIPE");
        return h;
    }
    for (int i = 0; i < 10; i++) {
        char path[64];
        snprintf(path, sizeof path, "\\\\.\\pipe\\discord-ipc-%d", i);
        Conn h = conn_try(path);
        if (h != NO_CONN) {
            snprintf(name, cap, "discord-ipc-%d", i);
            return h;
        }
    }
    return NO_CONN;
}

static int conn_write(Conn c, const void *p, size_t n) {
    const char *b = p;
    while (n) {
        DWORD w = 0;
        if (!WriteFile(c, b, (DWORD)n, &w, NULL) || !w) return 0;
        b += w, n -= w;
    }
    return 1;
}

/* The bytes waiting, without waiting for any (-1: the pipe is gone). No read of this thread's is ever
 * pending on the handle, so PeekNamedPipe does not wait behind one. */
static long conn_avail(Conn c) {
    DWORD a = 0;
    return PeekNamedPipe(c, NULL, 0, NULL, &a, NULL) ? (long)a : -1;
}

static int conn_read(Conn c, void *p, size_t n) { /* n bytes known to be waiting */
    DWORD r = 0;
    return ReadFile(c, p, (DWORD)n, &r, NULL) && r == n;
}

static void conn_close(Conn c) { CloseHandle(c); }
static int self_pid(void) { return (int)GetCurrentProcessId(); }
#else
typedef int Conn;
#define NO_CONN (-1)
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 /* macOS: SO_NOSIGPIPE on the socket instead */
#endif

static Conn conn_try(const char *path) {
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof a.sun_path || access(path, F_OK)) return NO_CONN;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return NO_CONN;
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) return fd;
    close(fd);
    return NO_CONN;
}

static Conn conn_open(char *name, size_t cap) {
    char pipe[512];
    if (test_endpoint(pipe, sizeof pipe)) {
        Conn fd = conn_try(pipe);
        if (fd != NO_CONN) snprintf(name, cap, "MT_DISCORD_PIPE");
        return fd;
    }
    const char *dirs[6];
    int nd = 0;
    static const char *const envs[] = {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"};
    for (size_t i = 0; i < sizeof envs / sizeof *envs; i++) {
        const char *d = getenv(envs[i]);
        if (d && *d) dirs[nd++] = d;
    }
    dirs[nd++] = "/tmp";
    static const char *const subs[] = {"", "app/com.discordapp.Discord/", "app/com.discordapp.DiscordCanary/",
                                       "snap.discord/", "snap.discord-canary/", ".flatpak/dev.vencord.Vesktop/xdg-run/"};
    for (int i = 0; i < 10; i++)
        for (int d = 0; d < nd; d++)
            for (size_t s = 0; s < sizeof subs / sizeof *subs; s++) {
                char path[512];
                snprintf(path, sizeof path, "%s/%sdiscord-ipc-%d", dirs[d], subs[s], i);
                Conn fd = conn_try(path);
                if (fd != NO_CONN) {
                    snprintf(name, cap, "%sdiscord-ipc-%d", subs[s], i); /* not the folder: the log stays private */
                    return fd;
                }
            }
    return NO_CONN;
}

static int conn_write(Conn c, const void *p, size_t n) {
    const char *b = p;
    while (n) {
        ssize_t w = send(c, b, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return 0;
        b += w, n -= (size_t)w;
    }
    return 1;
}

static long conn_avail(Conn c) {
    struct pollfd p = {c, POLLIN, 0};
    if (poll(&p, 1, 0) < 0) return 0;
    if (p.revents & (POLLERR | POLLNVAL)) return -1;
    if (!(p.revents & (POLLIN | POLLHUP))) return 0;
    int n = 0;
    if (ioctl(c, FIONREAD, &n) < 0) return -1;
    return n > 0 ? n : -1; /* readable with nothing in it: closed */
}

static int conn_read(Conn c, void *p, size_t n) {
    char *b = p;
    while (n) {
        ssize_t r = recv(c, b, n, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return 0;
        b += r, n -= (size_t)r;
    }
    return 1;
}

static void conn_close(Conn c) { close(c); }
static int self_pid(void) { return (int)getpid(); }
#endif

/* ---- frames ---- */
static Conn conn = NO_CONN;
static char in[8192]; /* the latest frame's JSON (cut short past this; NUL-terminated) */

static int send_frame(uint32_t op, const char *json) {
    const size_t n = strlen(json);
    char *f = malloc(8 + n);
    if (!f) return 0;
    const uint32_t len = (uint32_t)n; /* little-endian, as every computer this runs on */
    memcpy(f, &op, 4), memcpy(f + 4, &len, 4), memcpy(f + 8, json, n);
    const int ok = conn_write(conn, f, 8 + n);
    free(f);
    return ok;
}

/* A frame, if one comes within wait_ms: 1 (its opcode in *op, its JSON in `in`), 0 none, -1 the connection
 * is gone. */
static int recv_frame(uint32_t *op, int wait_ms) {
    long a;
    for (int waited = 0; (a = conn_avail(conn)) < 8; waited += 10) {
        if (a < 0) return -1;
        if (waited >= wait_ms) return 0;
        nap(10);
    }
    unsigned char h[8];
    uint32_t len;
    if (!conn_read(conn, h, 8)) return -1;
    memcpy(op, h, 4), memcpy(&len, h + 4, 4);
    if (len > (1u << 20)) return -1;
    size_t got = 0;
    for (int waited = 0; got < len;) { /* the app writes a frame at once: the rest is there or nearly */
        if ((a = conn_avail(conn)) < 0) return -1;
        if (!a) {
            if ((waited += 5) > 2000) return -1;
            nap(5);
            continue;
        }
        char tmp[4096];
        size_t k = len - got < (size_t)a ? len - got : (size_t)a;
        if (k > sizeof tmp) k = sizeof tmp;
        if (!conn_read(conn, tmp, k)) return -1;
        if (got < sizeof in - 1) memcpy(in + got, tmp, got + k < sizeof in - 1 ? k : sizeof in - 1 - got);
        got += k;
    }
    in[len < sizeof in - 1 ? len : sizeof in - 1] = 0;
    return 1;
}

/* A JSON string's text from `in` ("message": "..."), for the log. */
static void json_field(const char *key, char *out, size_t cap) {
    char pattern[40];
    snprintf(pattern, sizeof pattern, "\"%s\":", key);
    const char *p = strstr(in, pattern);
    size_t k = 0;
    if (p) {
        for (p += strlen(pattern); *p == ' '; p++) {}
        if (*p == '"')
            for (p++; *p && *p != '"' && k + 1 < cap; p++) {
                if (*p == '\\' && p[1]) p++;
                out[k++] = *p;
            }
    }
    out[k] = 0;
}

/* s as the inside of a JSON string. */
static void json_escape(char *out, size_t cap, const char *s) {
    size_t k = 0;
    for (; *s && k + 7 < cap; s++) {
        const unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') out[k++] = '\\', out[k++] = (char)ch;
        else if (ch < 0x20) k += (size_t)snprintf(out + k, cap - k, "\\u%04x", ch);
        else out[k++] = (char)ch;
    }
    out[k] = 0;
}

/* ---- the status ---- */
static int send_status(const char *details, long long start_s, int nonce) {
    char d[256], j[1024];
    json_escape(d, sizeof d, details);
    snprintf(j, sizeof j,
             "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%d,\"activity\":{\"details\":\"%s\","
             "\"timestamps\":{\"start\":%lld},\"assets\":{\"large_image\":\"icon\",\"large_text\":"
             "\"GODZILLA (PS4) on PC: Minus Two\"}}},\"nonce\":\"%d\"}",
             self_pid(), d, start_s, nonce);
    return send_frame(OP_FRAME, j);
}

static void drop(void) {
    if (conn != NO_CONN) conn_close(conn);
    conn = NO_CONN;
}

static const char *app_id(void) {
    const char *e = getenv("MT_DISCORD_APP_ID");
    return e && *e ? e : DISCORD_APP_ID;
}

static void *discord_thread(void *unused) {
    (void)unused;
    runtime_thread_attach_host("Discord status");
    const char *id = app_id();
    char name[96] = "", sent[128] = "";
    int sent_any = 0, said_missing = 0, said_refused = 0, said_off = 0, nonce = 0, errors = 0;
    uint64_t next_try = 0, last_send = 0;
    const long long start_s = (long long)time(NULL); /* the profile's "elapsed": since the game started */
    for (;;) {
        const uint64_t now = now_ms();
        if (!mtgpu_discord_status()) {
            if (!said_off) puts("[discord] off (Discord status in the menu)");
            said_off = 1;
            if (conn != NO_CONN) { /* turned off: cleared at once */
                char j[128];
                snprintf(j, sizeof j, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%d},\"nonce\":\"%d\"}", self_pid(),
                         ++nonce);
                send_frame(OP_FRAME, j);
                drop();
                puts("[discord] off: the status is cleared");
            }
            mtgpu_discord_state(MTGPU_DISCORD_OFF);
            next_try = 0;
            nap(500);
            continue;
        }
        said_off = 0;
        if (conn == NO_CONN) {
            if (!said_refused) mtgpu_discord_state(MTGPU_DISCORD_LOOKING);
            if (now < next_try) { nap(500); continue; }
            next_try = now + 20000;
            if ((conn = conn_open(name, sizeof name)) == NO_CONN) {
                if (!said_missing) puts("[discord] the Discord app is not running: looking for it every 20 s");
                said_missing = 1;
                continue;
            }
            char j[160];
            uint32_t op = 0;
            snprintf(j, sizeof j, "{\"v\":1,\"client_id\":\"%s\"}", id);
            const int r = send_frame(OP_HANDSHAKE, j) ? recv_frame(&op, 5000) : -1;
            if (r > 0 && op == OP_FRAME && strstr(in, "\"READY\"")) { /* its user is not logged: privacy */
                printf("[discord] connected to the Discord app (%s)\n", name);
                mtgpu_discord_state(MTGPU_DISCORD_CONNECTED);
                said_missing = said_refused = sent_any = 0;
                errors = 0;
            } else {
                char message[160];
                json_field("message", message, sizeof message);
                if (r > 0 && op == OP_CLOSE) {
                    if (!said_refused) printf("[discord] the Discord app said no: %s\n", message[0] ? message : "(no reason)");
                    said_refused = 1;
                    mtgpu_discord_state(MTGPU_DISCORD_REFUSED);
                } else {
                    printf("[discord] no answer from the Discord app (%s)\n", name);
                }
                drop();
                continue;
            }
        }
        /* what came in: pings to answer, errors, the app closing */
        uint32_t op = 0;
        int r;
        while ((r = recv_frame(&op, 0)) > 0) {
            if (op == OP_PING) send_frame(OP_PONG, in);
            else if (op == OP_CLOSE) { r = -1; break; }
            else if (op == OP_FRAME && strstr(in, "\"evt\":\"ERROR\"") && errors < 20) {
                char message[160];
                json_field("message", message, sizeof message);
                printf("[discord] the Discord app: %s%s\n", message, ++errors == 20 ? " (no more of these)" : "");
            }
        }
        if (r < 0) {
            puts("[discord] the Discord app closed the connection: looking for it every 20 s");
            drop();
            said_missing = 1;
            next_try = now + 20000;
            continue;
        }
        char details[128];
        runtime_state_text(details, sizeof details);
        if ((!sent_any || strcmp(details, sent)) && (!sent_any || now - last_send >= 5000)) {
            if (!send_status(details, start_s, ++nonce)) {
                puts("[discord] the Discord app closed the connection: looking for it every 20 s");
                drop();
                continue;
            }
            last_send = now, sent_any = 1;
            snprintf(sent, sizeof sent, "%s", details);
            printf("[discord] status: %s\n", details);
        }
        nap(250);
    }
    return NULL;
}

void runtime_discord_start(void) {
    static int started;
    if (started) return;
    started = 1;
    const char *pad = getenv("MT_PAD_FILE"), *background = getenv("MT_WINDOW_BACKGROUND"),
               *named = getenv("MT_DISCORD_APP_ID");
    if (((pad && *pad) || (background && background[0] == '1')) && !(named && *named)) {
        mtgpu_discord_state(MTGPU_DISCORD_TEST);
        puts("[discord] off in a scripted test run");
        return;
    }
    if (!*app_id()) {
        mtgpu_discord_state(MTGPU_DISCORD_NO_APP);
        puts("[discord] no Discord application in this build: no Discord status");
        return;
    }
    pthread_t thread;
    if (pthread_create(&thread, NULL, discord_thread, NULL) == 0) pthread_detach(thread);
}
