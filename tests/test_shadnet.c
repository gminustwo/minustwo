/* shadNet client (src/runtime_shadnet.c) against a scripted server on loopback: sign-in, the session callback
 * when the server closes a signed-in connection or stops answering the keep-alive, signing in again, and a
 * refused sign-in whose connection, closed by the server later, must not take the next session down with it. */
#undef NDEBUG /* the checks are asserts; release builds define NDEBUG */
#define SHADNET_KEEPALIVE_MS 300
#define SHADNET_REPLY_TIMEOUT_MS 600
#include "../src/runtime_shadnet.c"
#include <assert.h>

void runtime_thread_attach_host(const char *name) { (void)name; }

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

/* ---- the client's session callback ---- */
static int sessions_started, sessions_ended;
static void on_session(int up) { __atomic_add_fetch(up ? &sessions_started : &sessions_ended, 1, __ATOMIC_SEQ_CST); }
static int load(const int *value) { return __atomic_load_n(value, __ATOMIC_SEQ_CST); }

static int wait_for(const int *value, int expected, unsigned ms) {
    for (unsigned waited = 0; load(value) != expected && waited < ms; waited += 10) sleep_ms(10);
    return load(value) == expected;
}

/* ---- the server ---- */
enum { MAX_CONNECTIONS = 16 };
static pthread_mutex_t server_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t server_changed = PTHREAD_COND_INITIALIZER;
static NetSocket listener = INVALID_SOCKET;
static NetSocket connections[MAX_CONNECTIONS];
static int accepted, open_connection_count;
static int refused[MAX_CONNECTIONS], released[MAX_CONNECTIONS], still_open[MAX_CONNECTIONS];
static int refuse_login, answer_keepalive = 1;

static void reply(NetSocket s, uint16_t command, uint64_t id, const uint8_t *payload, uint32_t size) {
    uint8_t packet[HEADER_SIZE + 16];
    assert(size <= 16);
    packet[0] = TYPE_REPLY;
    put_u16(packet + 1, command);
    put_u32(packet + 3, HEADER_SIZE + size);
    put_u64(packet + 7, id);
    memcpy(packet + HEADER_SIZE, payload, size);
    send_all(s, packet, HEADER_SIZE + size);
}

/* One client connection: the version greeting, then answers. A connection whose sign-in was refused is held
 * open after the client let go of it, until the test releases it (a server slow to close it). */
static void *serve(void *arg) {
    int i = (int)(intptr_t)arg;
    NetSocket s = connections[i];
    uint8_t hello[HEADER_SIZE + 4] = {TYPE_SERVER_INFO};
    put_u32(hello + 3, HEADER_SIZE + 4);
    put_u32(hello + HEADER_SIZE, 1);
    send_all(s, hello, sizeof(hello));
    uint8_t header[HEADER_SIZE], payload[512];
    while (!recv_all(s, header, sizeof(header))) {
        uint32_t size = get_u32(header + 3);
        if (size < HEADER_SIZE || size - HEADER_SIZE > sizeof(payload) || recv_all(s, payload, size - HEADER_SIZE)) break;
        uint16_t command = get_u16(header + 1);
        uint64_t id = get_u64(header + 7);
        pthread_mutex_lock(&server_lock);
        int refuse = refuse_login, answer = answer_keepalive;
        if (command == CMD_LOGIN && refuse) refused[i] = 1;
        pthread_mutex_unlock(&server_lock);
        if (command == CMD_LOGIN) {
            static const uint8_t ok[] = {0, 2, 0, 0, 0, 0x10, 42}; /* error 0, LoginReply {user_id: 42} */
            static const uint8_t no[] = {ERR_LOGIN_INVALID_PASSWORD};
            if (refuse) reply(s, command, id, no, sizeof(no));
            else reply(s, command, id, ok, sizeof(ok));
        } else if (command != CMD_GET_SERVER_FEATURES || answer) {
            static const uint8_t ok[] = {0};
            reply(s, command, id, ok, sizeof(ok));
        }
    }
    pthread_mutex_lock(&server_lock);
    while (refused[i] && !released[i]) pthread_cond_wait(&server_changed, &server_lock);
    still_open[i] = 0;
    --open_connection_count;
    pthread_cond_broadcast(&server_changed);
    pthread_mutex_unlock(&server_lock);
    close_socket(s);
    return NULL;
}

static void *listen_main(void *unused) {
    (void)unused;
    for (;;) {
        NetSocket s = accept(listener, NULL, NULL);
        if (s == INVALID_SOCKET) return NULL;
        pthread_mutex_lock(&server_lock);
        assert(accepted < MAX_CONNECTIONS);
        int i = accepted++;
        connections[i] = s;
        still_open[i] = 1;
        ++open_connection_count;
        pthread_cond_broadcast(&server_changed);
        pthread_mutex_unlock(&server_lock);
        pthread_t thread;
        pthread_create(&thread, NULL, serve, (void *)(intptr_t)i);
        pthread_detach(thread);
    }
}

/* The server closes connection i (and lets a held one go). */
static void server_close(int i) {
    pthread_mutex_lock(&server_lock);
    if (still_open[i]) shutdown(connections[i], SHUTDOWN_BOTH);
    released[i] = 1;
    pthread_cond_broadcast(&server_changed);
    pthread_mutex_unlock(&server_lock);
}

static int server_accepted(void) {
    pthread_mutex_lock(&server_lock);
    int n = accepted;
    pthread_mutex_unlock(&server_lock);
    return n;
}

static uint16_t start_server(void) {
    sockets_init();
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(listener != INVALID_SOCKET);
    struct sockaddr_in at;
    memset(&at, 0, sizeof(at));
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(!bind(listener, (struct sockaddr *)&at, sizeof(at)) && !listen(listener, 8));
    socklen_t length = sizeof(at);
    assert(!getsockname(listener, (struct sockaddr *)&at, &length));
    pthread_t thread;
    pthread_create(&thread, NULL, listen_main, NULL);
    pthread_detach(thread);
    return ntohs(at.sin_port);
}

/* A request through the client (signing in when needed): 0 when answered without error. */
static int ask(void) {
    uint8_t error = 0xFF;
    return shadnet_request(CMD_GET_SERVER_FEATURES, NULL, &error, NULL, NULL) || error ? -1 : 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char server[32];
    snprintf(server, sizeof(server), "127.0.0.1:%u", start_server());
    set_env("MT_SHADNET_SERVER", server);
    set_env("MT_SHADNET_USER", "tester");
    set_env("MT_SHADNET_PASSWORD", "secret");
    shadnet_set_session(on_session);

    /* sign-in: one session, the account from the reply */
    assert(shadnet_sign_in() == 0);
    assert(load(&sessions_started) == 1 && load(&sessions_ended) == 0 && server_accepted() == 1);
    assert(shadnet_user_id() == 42);
    assert(ask() == 0);

    /* the server closes the signed-in connection: the session ends once; the next request signs in again */
    server_close(0);
    assert(wait_for(&sessions_ended, 1, 3000));
    assert(ask() == 0);
    assert(load(&sessions_started) == 2 && server_accepted() == 2);

    /* the server stops answering (the connection stays up): the keep-alive notices, the client lets go */
    pthread_mutex_lock(&server_lock);
    answer_keepalive = 0;
    pthread_mutex_unlock(&server_lock);
    assert(wait_for(&sessions_ended, 2, 5000));
    pthread_mutex_lock(&server_lock);
    answer_keepalive = 1;
    pthread_mutex_unlock(&server_lock);
    assert(wait_for(&open_connection_count, 0, 3000)); /* the client's end is closed, not left open */
    assert(ask() == 0);
    assert(load(&sessions_started) == 3 && server_accepted() == 3);
    sleep_ms(SHADNET_REPLY_TIMEOUT_MS + 100);
    assert(load(&sessions_ended) == 2); /* told once */

    /* a refused sign-in: no session. The server holds that connection open after the client let go of it,
     * and closes it only once the client is signed in on the next one: that session must live on. */
    server_close(2);
    assert(wait_for(&sessions_ended, 3, 3000));
    pthread_mutex_lock(&server_lock);
    refuse_login = 1;
    pthread_mutex_unlock(&server_lock);
    assert(shadnet_sign_in() == -1);
    assert(load(&sessions_started) == 3 && load(&sessions_ended) == 3 && server_accepted() == 4);
    pthread_mutex_lock(&server_lock);
    refuse_login = 0;
    pthread_mutex_unlock(&server_lock);
    assert(shadnet_sign_in() == 0);
    assert(load(&sessions_started) == 4 && server_accepted() == 5);
    sleep_ms(100);
    server_close(3);
    sleep_ms(2 * SHADNET_KEEPALIVE_MS);
    assert(load(&sessions_ended) == 3);
    assert(ask() == 0);
    assert(server_accepted() == 5); /* still the same connection */

    puts("shadnet-test: ok");
    return 0;
}
