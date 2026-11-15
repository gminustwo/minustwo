#ifndef MT_RUNTIME_H
#define MT_RUNTIME_H
#include <stdint.h>
#include <stddef.h>
#ifdef __APPLE__
#include "compat_mac.h"
#endif
#ifdef _WIN32
#include "compat_win.h"
/* No signals on Windows: the exception handler resumes the thread in mt_longjmp
 * (gpu/shim/mt_toggles.h uses the same buffer type). */
typedef mt_jmp_buf sigjmp_buf;
#define sigsetjmp(buffer, save) mt_setjmp(buffer)
#else
#include <setjmp.h>
#endif
/* Recovery point for speculative guest memory reads on this thread (probe.c fault handler). */
extern __thread sigjmp_buf *runtime_fault_recover;
/* Restarts the game (in-game settings menu, render resolution change). */
void runtime_restart(void);
#define ABI __attribute__((sysv_abi))
typedef void (ABI *GuestCallback)(void);
void runtime_start(uint64_t capabilities);
uintptr_t runtime_resolve(const char *name, int is_data);
void runtime_report(void);
/* Called at the end of runtime_report (the loader adds its own counters). */
void runtime_set_report_hook(void (*hook)(void));
void runtime_finalize(void *dso);
uintptr_t runtime_mutex_resolve(const char *name);
void runtime_mutex_report(void);
uintptr_t runtime_memory_resolve(const char *name);
void runtime_memory_report(void);
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
const char *runtime_import_name(const char *name);
uintptr_t runtime_rwlock_resolve(const char *name);
void runtime_rwlock_report(void);
void runtime_set_libc_tls(const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_module_tls(uint64_t module, const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_procparam(void *param);
void **runtime_application_heap_api(void);
uintptr_t runtime_thread_resolve(const char *name);
void runtime_thread_report(void);
/* Counts a guest sched_yield (zero_sleep 0) or a zero-length sleep (1) for the report (#209). */
void runtime_thread_count_yield(int zero_sleep);
void runtime_set_main_tls(const void *data, uint64_t filesz, uint64_t memsz, uint64_t align);
void runtime_thread_attach_main(void);
int32_t *runtime_errno(void);
uintptr_t runtime_sema_resolve(const char *name);
uintptr_t runtime_eventflag_resolve(const char *name);
uintptr_t runtime_ngs2_resolve(const char *name);
uintptr_t runtime_font_resolve(const char *name);
void runtime_font_report(void);
void runtime_ngs2_report(void);
void runtime_eventflag_report(void);
void runtime_sema_report(void);
unsigned runtime_sema_waiters(uint32_t id);
uintptr_t runtime_time_resolve(const char *name);
void runtime_content_configure(const uint32_t values[5]);
uintptr_t runtime_content_resolve(const char *name);
void runtime_content_report(void);
typedef struct { const char *name; void *function; } RuntimeExport;
#define RUNTIME_LOOKUP(table, nid) runtime_lookup(table, sizeof(table)/sizeof(*(table)), nid)
const char *runtime_symbol(const char *nid);
/* MT_IMPORT_TRACE (runtime_trace.c): `target` itself, or a trampoline that logs its calls. */
uintptr_t runtime_trace_wrap(const char *symbol, uintptr_t target);
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid);
uintptr_t runtime_kernel_resolve(const char *name);
uintptr_t runtime_file_resolve(const char *name);
uintptr_t runtime_services_resolve(const char *name);
uintptr_t runtime_np_resolve(const char *name);
int runtime_online_in_room(void); /* in an online VS room (lobby or match): runtime_cheats.c pauses fight cheats */

/* shadNet (runtime_shadnet.c): the community PSN server, when MT_SHADNET_SERVER is set. */
typedef struct { uint8_t *data; size_t size, capacity; int failed; } PbOut; /* protobuf writer */
typedef struct { const uint8_t *at, *end; } PbIn;                             /* protobuf reader */
void pb_uint(PbOut *out, unsigned field, uint64_t value);   /* varint field (skipped when 0) */
void pb_bool(PbOut *out, unsigned field, int value);
void pb_bytes(PbOut *out, unsigned field, const void *data, size_t size); /* skipped when empty */
void pb_str(PbOut *out, unsigned field, const char *text);
void pb_msg(PbOut *out, unsigned field, const PbOut *inner); /* nested message, written even when empty */
void pb_free(PbOut *out);
/* The next field: 1 with its number, and its value (varint or fixed) or its bytes (length-delimited);
 * 0 at the end or on malformed input. */
int pb_next(PbIn *in, unsigned *field, uint64_t *value, const uint8_t **data, size_t *size);
typedef void (*ShadnetNotify)(uint16_t type, const uint8_t *payload, uint32_t size);
int shadnet_enabled(void);
int runtime_online_debug(void); /* MT_ONLINE_DEBUG=1: the online code's detailed log */
/* Connects and signs in (once; again after a lost connection): 0, or -1 with the reason logged. */
int shadnet_sign_in(void);
const char *shadnet_npid(void);
uint64_t shadnet_user_id(void);
/* Server address (dotted IPv4, network order) and UDP signaling port, after a successful sign-in. */
uint32_t shadnet_server_ip(void);
uint16_t shadnet_signaling_port(void);
void shadnet_set_notify(ShadnetNotify notify);
/* One command: its proto `request` (may be NULL), the server's error byte in *error, the reply's proto blob in
 * *reply (malloc'ed, may be NULL) and *reply_size. 0, or -1 when there was no answer. */
int shadnet_request(uint16_t command, const PbOut *request, uint8_t *error, uint8_t **reply, uint32_t *reply_size);
/* The same with raw bytes before the proto (score commands: the communication ID), answering with everything
 * after the reply's error byte in *raw. */
int shadnet_request_raw(uint16_t command, const void *prefix, uint32_t prefix_size, const PbOut *request, uint8_t *error,
                        uint8_t **raw, uint32_t *raw_size);
uintptr_t runtime_score_resolve(const char *name); /* NpScore leaderboards through shadNet (runtime_score.c) */

/* The port's online protocol: copies that differ never match each other (runtime_np.c's match key) and
 * ignore each other's P2P datagrams (runtime_p2p.c). Raise it with any incompatible change. */
#define MT_ONLINE_PROTOCOL 1

/* P2P sockets and Rudp between copies of the port (runtime_p2p.c), with shadNet. */
uintptr_t runtime_p2p_resolve(const char *name);
int p2p_start(void);
void p2p_set_established(void (*cb)(uint16_t member, int up));
void p2p_room_joined(uint64_t room, uint16_t member);
void p2p_member(uint16_t member, const char *npid, uint32_t public_address, uint16_t port);
void p2p_member_left(uint16_t member);
void p2p_room_left(void);
void p2p_room_message(const uint8_t *message, size_t size, uint16_t from);
int p2p_member_established(uint16_t member);
uint32_t p2p_virtual_address(uint16_t member); /* network order */
uint16_t p2p_game_port(void);                  /* network order */

void runtime_file_report(void);
void runtime_file_configure(const char *app0, const char *user);
int runtime_file_mount(const char *guest, const char *host);
void runtime_file_unmount(const char *guest);
int runtime_file_translate(const char *guest, char *out, size_t size);
int64_t runtime_file_open(const char *path, int flags, int mode);
int64_t runtime_file_close(int fd);
int64_t runtime_file_read(int fd, void *buffer, uint64_t size);
int64_t runtime_file_pread(int fd, void *buffer, uint64_t size, int64_t offset);
int64_t runtime_file_write(int fd, const void *buffer, uint64_t size);
int64_t runtime_file_lseek(int fd, int64_t offset, int whence);
int64_t runtime_file_stat(const char *path, void *guest_stat);
int64_t runtime_file_fstat(int fd, void *guest_stat);
int64_t runtime_file_getdents(int fd, char *buffer, uint64_t size, int64_t *base);
void runtime_thread_keys_cleanup(void);
/* Guest-visible errno values are FreeBSD's. */
int32_t runtime_guest_errno(int host_errno);
void *runtime_low_map(size_t size, int prot);
/* Windows and macOS: reserves the PS4 user address range so host allocations stay out of it
 * (no-op on Linux, where the host maps from the top of the address space). */
void runtime_memory_reserve(void);
uintptr_t runtime_ajm_resolve(const char *name);
void runtime_ajm_report(void);
/* The menu's cheats (runtime_cheats.c): configure finds the game's player table and kaiju records
 * in the loaded image; start, called once the image has its final protections, starts the thread
 * that keeps the cheats the menu turns on applied. */
void runtime_cheats_configure(unsigned char *image, uint64_t size);
void runtime_cheats_start(void);
/* The render thread's waits for the GPU (runtime_label_wait.c): its spin loops on the GPU's label
 * in the loaded image become calls to stubs written to `stubs` (within rel32 of the image) that
 * sleep until the GPU writes the label. Returns the number of loops replaced. */
enum { RUNTIME_LABEL_WAIT_STUB_AREA = 4096 };
unsigned runtime_label_wait_install(unsigned char *image, uint64_t size, unsigned char *stubs, size_t bytes);
/* AjmDecMp3ParseFrame (guest layout). */
typedef struct {
    uint64_t frame_size;
    uint32_t num_channels, samples_per_channel, bitrate, sample_rate, encoder_delay, num_frames, total_samples, ofl_type;
} Mp3Frame;
typedef struct Mp3Decoder Mp3Decoder;
int runtime_mp3_parse(const unsigned char *data, uint32_t size, int parse_ofl, int layer3_only, Mp3Frame *frame);
Mp3Decoder *runtime_mp3_open(void);
void runtime_mp3_close(Mp3Decoder *decoder);
void runtime_mp3_reset(Mp3Decoder *decoder);
int runtime_mp3_decode(Mp3Decoder *decoder, const unsigned char *data, int size, float *pcm, int max_samples, int *channels);
uintptr_t runtime_videodec_resolve(const char *name);
uintptr_t runtime_audio_resolve(const char *name);
void runtime_audio_report(void);
uintptr_t runtime_pad_resolve(const char *name);
void runtime_pad_report(void);
/* probe --gamepad: prints one "Gamepad:" line for the first pad found (returns 1) or none (0). */
int runtime_pad_describe(void);
uintptr_t runtime_rtc_resolve(const char *name);
const char *runtime_file_user_dir(void);
void runtime_savedata_configure(const char *title);
uintptr_t runtime_savedata_resolve(const char *name);
void runtime_savedata_report(void);
/* OrbisSaveDataParam (1328 bytes) of a save directory of the current title. */
#define RUNTIME_SAVE_PARAM_SIZE 1328
int runtime_savedata_param(int32_t user, const char *dir, void *param);
int runtime_savedata_icon(int32_t user, const char *dir, char *out, size_t size);
void runtime_savedata_preview_path(const char *path);
void runtime_thread_attach_host(const char *name);
#endif
