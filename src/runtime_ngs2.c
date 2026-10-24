/* libSceNgs2: the PS4's software voice mixer. GODZILLA plays every sound through it: sampler
 * voices decode ATRAC9 (LibAtrac9) or PCM waveform blocks, submixer voices collect them, and the
 * mastering voices are mixed into the buffers sceNgs2SystemRender hands back; the game sends those
 * to sceAudioOutOutput itself.
 *
 * What is mixed: waveform blocks with repeats and loops, pitch, envelopes, port volumes, matrices
 * and patches between voices, block-end callbacks. What passes through unchanged: filters,
 * distortion, compressors, limiters and EQ; reverb voices are silent (their send would otherwise
 * double the dry signal). MZ_NGS2_TRACE=1 logs racks, parameters and events as the game sends them.
 *
 * Handles are opaque numbers (type in the top byte), never host pointers. One recursive lock per
 * process serializes control and rendering: callbacks into the game run under it. */
#define _GNU_SOURCE
#include "runtime.h"
#include "third_party/LibAtrac9/C/src/libatrac9.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#define ERR_FAIL ((int32_t)0x804A0001)
#define ERR_INVALID_OUT_ADDRESS ((int32_t)0x804A0053)
#define ERR_INVALID_OPTION_ADDRESS ((int32_t)0x804A0080)
#define ERR_INVALID_HANDLE ((int32_t)0x804A0200)
#define ERR_INVALID_BUFFER_INFO ((int32_t)0x804A0206)
#define ERR_INVALID_BUFFER_ADDRESS ((int32_t)0x804A0207)
#define ERR_INVALID_SYSTEM_HANDLE ((int32_t)0x804A0230)
#define ERR_INVALID_RACK_ID ((int32_t)0x804A0260)
#define ERR_INVALID_RACK_HANDLE ((int32_t)0x804A0261)
#define ERR_INVALID_VOICE_HANDLE ((int32_t)0x804A0300)
#define ERR_INVALID_VOICE_INDEX ((int32_t)0x804A0302)
#define ERR_INVALID_EVENT_TYPE ((int32_t)0x804A0303)
#define ERR_INVALID_PORT_INDEX ((int32_t)0x804A0304)
#define ERR_INVALID_WAVEFORM_FORMAT ((int32_t)0x804A0400)
#define ERR_INVALID_WAVEFORM_TYPE ((int32_t)0x804A0402)
#define ERR_INVALID_WAVEFORM_DATA ((int32_t)0x804A0408)
#define ERR_INVALID_MATRIX_INDEX ((int32_t)0x804A0700)

#define RACK_SAMPLER 0x1000
#define RACK_SUBMIXER 0x2000
#define RACK_REVERB 0x2001
#define RACK_EQ 0x2002
#define RACK_MASTERING 0x3000
#define RACK_CUSTOM_SAMPLER 0x4001
#define RACK_CUSTOM_SUBMIXER 0x4002
#define RACK_CUSTOM_MASTERING 0x4003
enum { KIND_SAMPLER, KIND_SUBMIXER, KIND_REVERB, KIND_EQ, KIND_MASTERING };

/* Parameters common to every voice (OrbisNgs2VoiceParamHeader.id). */
#define PARAM_MATRIX_LEVELS 1
#define PARAM_PORT_VOLUME 2
#define PARAM_PORT_MATRIX 3
#define PARAM_PORT_DELAY 4
#define PARAM_PATCH 5
#define PARAM_EVENT 6
#define PARAM_CALLBACK 7
/* Rack-specific parameters: the rack id in the top half, the parameter (from 0) in the bottom
 * half; 0x30000005 is the mastering voice's output. */
#define SAMPLER_SETUP 0
#define SAMPLER_WAVEFORM_BLOCKS 1
#define SAMPLER_WAVEFORM_ADDRESS 2
#define SAMPLER_WAVEFORM_FRAME_OFFSET 3
#define SAMPLER_EXIT_LOOP 4
#define SAMPLER_PITCH 5
#define SAMPLER_ENVELOPE 6
#define MIXER_SETUP 0
#define MASTERING_OUTPUT 5

#define EVENT_PLAY 0
#define EVENT_STOP 1
#define EVENT_STOP_IMMEDIATE 2
#define EVENT_KILL 3
#define EVENT_PAUSE 4
#define EVENT_RESUME 5

#define STATE_IN_USE 0x01
#define STATE_PLAYING 0x02
#define STATE_PAUSED 0x04
#define STATE_STOPPED 0x08
#define STATE_ERROR 0x10
#define STATE_EMPTY 0x20

#define WAVE_PCM_I8 0x10
#define WAVE_PCM_U8 0x11
#define WAVE_PCM_I16L 0x12
#define WAVE_PCM_I16B 0x13
#define WAVE_PCM_I24L 0x14
#define WAVE_PCM_I32L 0x16
#define WAVE_PCM_F32L 0x18
#define WAVE_ATRAC9 0x40

#define CALLBACK_BLOCK_END 0x1

#define MAX_CHANNELS 8
#define MAX_GRAIN 1024
#define MAX_PORTS 16
#define MAX_MATRICES 16
#define MAX_BLOCKS 64
#define MAX_POINTS 32
#define INFINITE_REPEATS 0x7fffffffu

typedef uint64_t Handle;
enum { HANDLE_SYSTEM=1, HANDLE_RACK=2, HANDLE_VOICE=3 };

typedef struct { uint32_t type, channels, rate, config, frame_offset, frame_margin; } WaveFormat;
typedef struct { uint32_t offset, size, repeats, skip, samples, reserved; uint64_t user; } WaveBlock;
typedef struct { uint32_t curve, duration; float height; } EnvPoint;
typedef struct { void *buffer; uint64_t size; uint64_t reserved[5]; uint64_t user; } BufferInfo;
typedef struct { uint16_t size; int16_t next; uint32_t id; } ParamHeader;
_Static_assert(sizeof(WaveFormat)==24 && sizeof(WaveBlock)==32 && sizeof(EnvPoint)==12, "Ngs2 waveform layouts");
_Static_assert(sizeof(BufferInfo)==64 && sizeof(ParamHeader)==8, "Ngs2 buffer and parameter layouts");

struct Rack;
typedef struct Voice {
    struct Rack *rack;
    Handle handle;
    uint32_t index, flags;
    int channels;                  /* output channels: the waveform's, or the mixer voice's setup */
    struct { struct Voice *dest; uint32_t input; float volume; int32_t matrix; } ports[MAX_PORTS];
    float matrix[MAX_MATRICES][MAX_CHANNELS*MAX_CHANNELS];
    uint32_t matrix_levels[MAX_MATRICES];
    float input[MAX_GRAIN*MAX_CHANNELS]; /* mixer voices: what the patches brought this grain */
    int depth;                     /* mixer voices: patches between it and a mastering voice */
    uint32_t output;               /* mastering voices: the render buffer they are mixed into */
    float limiter;                 /* mastering voices: the limiter's gain, 1 when nothing is too loud */
    /* Sampler: the waveform and where playback is in it. */
    WaveFormat format;
    const unsigned char *data;
    WaveBlock blocks[MAX_BLOCKS];
    uint32_t block_count, block, repeated, block_left;
    const unsigned char *read;
    uint32_t read_left, skip_left;
    int exit_loop, ended;
    void *at9;
    Atrac9CodecInfo at9_info;
    uint32_t at9_frames, at9_superframe_left;
    float pcm[256*MAX_CHANNELS];   /* decoded frames waiting to be resampled */
    uint32_t pcm_count, pcm_pos;
    double frac;
    float prev[MAX_CHANNELS], cur[MAX_CHANNELS];
    float pitch;
    EnvPoint points[MAX_POINTS];
    uint32_t forward_points, release_points, env_point, env_pos;
    float env_level, env_start;
    int env_active, releasing;
    void (ABI *callback)(const void *info);
    uint64_t callback_data;
    uint32_t callback_flags;
    uint64_t decoded_samples, decoded_bytes;
} Voice;
typedef struct Rack {
    struct System *system;
    Handle handle;
    uint32_t id, kind, max_voices, max_ports, max_matrices;
    char name[16];
    BufferInfo buffer;
    Voice *voices;
    struct Rack *next;
} Rack;
typedef struct System {
    Handle handle;
    char name[16];
    uint32_t sample_rate, grain;
    BufferInfo buffer;
    Rack *racks;
    uint64_t renders;
    struct System *next;
} System;

static pthread_mutex_t lock=PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static System *systems;
static uint32_t next_serial=1;
static size_t renders, voices_played, blocks_finished, decode_errors;
static int trace=-1;
static int tracing(void) { if (trace<0) { const char *v=getenv("MZ_NGS2_TRACE"); trace=v && *v && *v!='0'; } return trace; }

static Handle make_handle(int type) { return (Handle)type<<56 | next_serial++; }
static int handle_type(Handle h) { return (int)(h>>56); }
static System *find_system(Handle h) {
    if (handle_type(h)!=HANDLE_SYSTEM) return NULL;
    for (System *s=systems;s;s=s->next) if (s->handle==h) return s;
    return NULL;
}
static Rack *find_rack(Handle h) {
    if (handle_type(h)!=HANDLE_RACK) return NULL;
    for (System *s=systems;s;s=s->next) for (Rack *r=s->racks;r;r=r->next) if (r->handle==h) return r;
    return NULL;
}
/* Voice handle: the rack's serial in bits 16..55, the voice index in the low 16 bits. */
static Voice *find_voice(Handle h) {
    if (handle_type(h)!=HANDLE_VOICE) return NULL;
    const Handle rack=(Handle)HANDLE_RACK<<56 | (h>>16 & 0xffffffffffull);
    Rack *r=find_rack(rack);
    const uint32_t index=(uint32_t)(h & 0xffff);
    return r && index<r->max_voices ? &r->voices[index] : NULL;
}
static int rack_kind(uint32_t id) {
    switch (id) {
    case RACK_SAMPLER: case RACK_CUSTOM_SAMPLER: return KIND_SAMPLER;
    case RACK_SUBMIXER: case RACK_CUSTOM_SUBMIXER: return KIND_SUBMIXER;
    case RACK_REVERB: return KIND_REVERB;
    case RACK_EQ: return KIND_EQ;
    case RACK_MASTERING: case RACK_CUSTOM_MASTERING: return KIND_MASTERING;
    default: return -1;
    }
}
static const char *kind_name(uint32_t kind) {
    static const char *names[]={"sampler","submixer","reverb","eq","mastering"};
    return kind<5 ? names[kind] : "?";
}
static uint32_t read32(const void *base, size_t offset) { uint32_t v; memcpy(&v,(const char *)base+offset,4); return v; }

/* ---- System ---- */
/* OrbisNgs2SystemOption: size, name[16], flags, maxGrainSamples, numGrainSamples, sampleRate. */
static void system_option(const unsigned char *option, uint32_t *grain, uint32_t *rate, char name[16]) {
    *grain=256; *rate=48000; name[0]=0;
    if (!option) return;
    uint64_t size; memcpy(&size,option,8);
    if (size<40) return;
    memcpy(name,option+8,16); name[15]=0;
    const uint32_t g=read32(option,32), r=read32(option,36);
    if (g>=64 && g<=MAX_GRAIN) *grain=g;
    if (r) *rate=r;
}
static ABI int32_t system_query_size(const unsigned char *option, BufferInfo *out) {
    (void)option;
    if (!out) return ERR_INVALID_BUFFER_INFO;
    out->size=64*1024; /* the game's memory is kept for it; the mixer state lives in the host heap */
    return 0;
}
static ABI int32_t system_create(const unsigned char *option, const BufferInfo *buffer, Handle *out) {
    if (!out) return ERR_INVALID_OUT_ADDRESS;
    if (!buffer) return ERR_INVALID_BUFFER_INFO;
    System *s=calloc(1,sizeof(*s));
    if (!s) return ERR_FAIL;
    system_option(option,&s->grain,&s->sample_rate,s->name);
    s->buffer=*buffer;
    pthread_mutex_lock(&lock);
    s->handle=make_handle(HANDLE_SYSTEM);
    s->next=systems; systems=s;
    *out=s->handle;
    pthread_mutex_unlock(&lock);
    printf("Runtime: Ngs2 system '%s' created: %u Hz, %u-sample grain\n",s->name,s->sample_rate,s->grain);
    return 0;
}
static void voice_release(Voice *v) {
    if (v->at9) { Atrac9ReleaseHandle(v->at9); v->at9=NULL; }
}
static void rack_free(Rack *r) {
    for (uint32_t i=0;i<r->max_voices;++i) voice_release(&r->voices[i]);
    free(r->voices); free(r);
}
/* Patches into a rack about to go away would point at freed voices. */
static void unpatch_rack(System *s, const Rack *gone) {
    for (Rack *r=s->racks;r;r=r->next) for (uint32_t i=0;i<r->max_voices;++i)
        for (int p=0;p<MAX_PORTS;++p) if (r->voices[i].ports[p].dest && r->voices[i].ports[p].dest->rack==gone)
            r->voices[i].ports[p].dest=NULL;
}
static ABI int32_t system_destroy(Handle handle, BufferInfo *out) {
    pthread_mutex_lock(&lock);
    System **p=&systems;
    while (*p && (*p)->handle!=handle) p=&(*p)->next;
    if (!*p) { pthread_mutex_unlock(&lock); return ERR_INVALID_SYSTEM_HANDLE; }
    System *s=*p; *p=s->next;
    while (s->racks) { Rack *r=s->racks; s->racks=r->next; rack_free(r); }
    pthread_mutex_unlock(&lock);
    if (out) *out=s->buffer;
    free(s);
    return 0;
}

/* ---- Racks ---- */
typedef struct { uint32_t max_voices, max_ports, max_matrices; char name[16]; } RackOptions;
/* OrbisNgs2RackOption: size, name[16] @8, flags @24, maxGrainSamples @28, maxVoices @32,
 * maxInputDelayBlocks @36, maxMatrices @40, maxPorts @44. */
static RackOptions rack_options(const unsigned char *option) {
    RackOptions o={1,1,1,""};
    if (!option) return o;
    uint64_t size; memcpy(&size,option,8);
    if (size<48) return o;
    memcpy(o.name,option+8,16); o.name[15]=0;
    const uint32_t voices=read32(option,32), matrices=read32(option,40), ports=read32(option,44);
    if (voices) o.max_voices=voices>4096 ? 4096 : voices;
    if (matrices) o.max_matrices=matrices>MAX_MATRICES ? MAX_MATRICES : matrices;
    if (ports) o.max_ports=ports>MAX_PORTS ? MAX_PORTS : ports;
    return o;
}
static ABI int32_t rack_query_size(uint32_t id, const unsigned char *option, BufferInfo *out) {
    if (rack_kind(id)<0) return ERR_INVALID_RACK_ID;
    if (!out) return ERR_INVALID_BUFFER_INFO;
    out->size=16*1024+(uint64_t)rack_options(option).max_voices*1024;
    return 0;
}
static ABI int32_t rack_create(Handle system, uint32_t id, const unsigned char *option, const BufferInfo *buffer, Handle *out) {
    const int kind=rack_kind(id);
    if (kind<0) return ERR_INVALID_RACK_ID;
    if (!out) return ERR_INVALID_OUT_ADDRESS;
    if (!buffer) return ERR_INVALID_BUFFER_INFO;
    const RackOptions o=rack_options(option);
    if (tracing() && option) {
        printf("Ngs2 trace: rack 0x%x option:",id);
        for (int i=0;i<48;++i) printf(" %08x",read32(option,(size_t)i*4));
        putchar('\n');
    }
    Rack *r=calloc(1,sizeof(*r));
    Voice *voices=calloc(o.max_voices,sizeof(Voice));
    if (!r || !voices) { free(r); free(voices); return ERR_FAIL; }
    pthread_mutex_lock(&lock);
    System *s=find_system(system);
    if (!s) { pthread_mutex_unlock(&lock); free(r); free(voices); return ERR_INVALID_SYSTEM_HANDLE; }
    r->system=s; r->id=id; r->kind=(uint32_t)kind; r->max_voices=o.max_voices; r->max_ports=o.max_ports;
    r->max_matrices=o.max_matrices; memcpy(r->name,o.name,16); r->buffer=*buffer; r->voices=voices;
    r->handle=make_handle(HANDLE_RACK);
    for (uint32_t i=0;i<o.max_voices;++i) {
        Voice *v=&voices[i];
        v->rack=r; v->index=i; v->handle=(Handle)HANDLE_VOICE<<56 | (r->handle & 0xffffffffffull)<<16 | i;
        v->channels=kind==KIND_SAMPLER ? 1 : 2; v->pitch=1.0f;
        for (int p=0;p<MAX_PORTS;++p) { v->ports[p].volume=1.0f; v->ports[p].matrix=-1; }
    }
    /* Samplers first, mastering last: the order a grain is mixed in. */
    Rack **at=&s->racks;
    while (*at && rack_kind((*at)->id)<=kind) at=&(*at)->next;
    r->next=*at; *at=r;
    *out=r->handle;
    pthread_mutex_unlock(&lock);
    printf("Runtime: Ngs2 %s rack '%s' (0x%x): %u voices, %u ports, %u matrices\n",kind_name((uint32_t)kind),r->name,id,
           r->max_voices,r->max_ports,r->max_matrices);
    return 0;
}
static ABI int32_t rack_destroy(Handle handle, BufferInfo *out) {
    pthread_mutex_lock(&lock);
    Rack *r=find_rack(handle);
    if (!r) { pthread_mutex_unlock(&lock); return ERR_INVALID_RACK_HANDLE; }
    System *s=r->system;
    Rack **p=&s->racks;
    while (*p!=r) p=&(*p)->next;
    *p=r->next;
    unpatch_rack(s,r);
    if (out) *out=r->buffer;
    rack_free(r);
    pthread_mutex_unlock(&lock);
    return 0;
}
/* OrbisNgs2RackInfo (168 bytes; the sampler's adds 32): name, handle, buffer info, owner, type,
 * rack id, uid, grain limits, maxVoices @116, ..., maxPorts @132, state, ..., activeVoiceCount @160. */
static ABI int32_t rack_info(Handle handle, unsigned char *info, uint64_t size) {
    if (!info || size<168) return ERR_INVALID_OUT_ADDRESS;
    pthread_mutex_lock(&lock);
    Rack *r=find_rack(handle);
    if (!r) { pthread_mutex_unlock(&lock); return ERR_INVALID_RACK_HANDLE; }
    memset(info,0,size>4096 ? 4096 : size);
    memcpy(info,r->name,16);
    memcpy(info+16,&r->handle,8);
    memcpy(info+24,&r->buffer,64);
    memcpy(info+88,&r->system->handle,8);
    uint32_t active=0;
    for (uint32_t i=0;i<r->max_voices;++i) active+=(r->voices[i].flags & STATE_IN_USE)!=0;
    const uint32_t grain=r->system->grain;
    const uint32_t values[]={r->id,r->kind,(uint32_t)r->handle,64,MAX_GRAIN,r->max_voices,r->max_voices,1,r->max_matrices,r->max_ports};
    memcpy(info+96,values,sizeof(values));
    memcpy(info+160,&active,4);
    (void)grain;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t rack_voice(Handle handle, uint32_t index, Handle *out) {
    if (!out) return ERR_INVALID_OUT_ADDRESS;
    pthread_mutex_lock(&lock);
    Rack *r=find_rack(handle);
    int32_t result=!r ? ERR_INVALID_RACK_HANDLE : index>=r->max_voices ? ERR_INVALID_VOICE_INDEX : 0;
    if (!result) *out=r->voices[index].handle;
    pthread_mutex_unlock(&lock);
    if (tracing()) printf("Ngs2 trace: rack %llx voice %u -> 0x%08x\n",(unsigned long long)handle,index,(unsigned)result);
    return result;
}

/* ---- Sampler playback ---- */
static int sample_bytes(uint32_t type) {
    switch (type) {
    case WAVE_PCM_I8: case WAVE_PCM_U8: return 1;
    case WAVE_PCM_I16L: case WAVE_PCM_I16B: return 2;
    case WAVE_PCM_I24L: return 3;
    case WAVE_PCM_I32L: case WAVE_PCM_F32L: return 4;
    default: return 0;
    }
}
static int format_supported(const WaveFormat *f) {
    return f->channels>=1 && f->channels<=MAX_CHANNELS && f->rate>=1000 && f->rate<=192000 &&
           (f->type==WAVE_ATRAC9 || sample_bytes(f->type));
}
/* Starts block v->block: the reading position, the samples to skip and to play. ATRAC9 decoding
 * starts over from the block's first superframe. */
static void block_start(Voice *v) {
    const WaveBlock *b=&v->blocks[v->block];
    v->read=v->data ? v->data+b->offset : NULL;
    v->read_left=v->data ? b->size : 0;
    v->skip_left=b->skip;
    v->block_left=b->samples ? b->samples : UINT32_MAX;
    v->pcm_count=v->pcm_pos=0;
    if (v->format.type==WAVE_ATRAC9) {
        /* A fresh decoder: LibAtrac9's init keeps the superframe position and overlap of the
         * previous stream, which fails or clicks on the next sound of a reused voice. */
        if (v->at9) Atrac9ReleaseHandle(v->at9);
        v->at9=Atrac9GetHandle();
        unsigned char config[4]; memcpy(config,&v->format.config,4);
        if (!v->at9 || Atrac9InitDecoder(v->at9,config) || Atrac9GetCodecInfo(v->at9,&v->at9_info) ||
            v->at9_info.frameSamples>256 || v->at9_info.channels>MAX_CHANNELS) {
            ++decode_errors; v->read_left=0;
            if (tracing()) printf("Ngs2 trace: voice %llx: ATRAC9 config %08x rejected\n",(unsigned long long)v->handle,v->format.config);
        }
        v->at9_frames=0; v->at9_superframe_left=(uint32_t)v->at9_info.superframeSize;
    }
}
/* The game's callback for a finished block (OrbisNgs2VoiceCallbackInfo, 56 bytes). */
static void block_callback(Voice *v, const WaveBlock *b) {
    if (!v->callback || !(v->callback_flags & CALLBACK_BLOCK_END)) return;
    struct { uint64_t data, voice; uint32_t flag, reserved; uint64_t user; const void *address; uint32_t size, repeated, attributes, reserved2; } info={
        v->callback_data,v->handle,CALLBACK_BLOCK_END,0,b->user,v->data ? v->data+b->offset : NULL,b->size,v->repeated,0,0};
    _Static_assert(sizeof(info)==56,"OrbisNgs2VoiceCallbackInfo layout");
    v->callback(&info);
}
/* Decodes the next frames of the current block into v->pcm; 0 once the waveform has ended. */
static int refill(Voice *v) {
    for (int guard=0; guard<4*MAX_BLOCKS+8; ++guard) {
        if (v->block>=v->block_count) return 0;
        const WaveBlock *b=&v->blocks[v->block];
        if (!v->block_left || !v->read_left) {
            ++blocks_finished;
            const uint32_t repeats=b->repeats;
            const int again=!v->exit_loop && repeats && (repeats>=INFINITE_REPEATS || v->repeated<repeats);
            block_callback(v,b);
            if (again) ++v->repeated;
            else { ++v->block; v->repeated=0; if (v->block>=v->block_count) return 0; }
            block_start(v);
            continue;
        }
        const int channels=(int)v->format.channels;
        uint32_t frames=0;
        if (v->format.type==WAVE_ATRAC9) {
            static int16_t pcm[256*MAX_CHANNELS]; /* under the lock: guest stacks can be 64 KiB */
            int used=0;
            const int error=Atrac9Decode(v->at9,v->read,pcm,&used);
            if (error || used<=0 || (uint32_t)used>v->read_left) {
                if (tracing()) printf("Ngs2 trace: voice %llx: ATRAC9 frame rejected (error %d, %d bytes used, %u left, bytes %02x %02x %02x %02x)\n",
                                      (unsigned long long)v->handle,error,used,v->read_left,v->read[0],v->read[1],v->read[2],v->read[3]);
                ++decode_errors; v->read_left=0; continue;
            }
            v->read+=used; v->read_left-=(uint32_t)used; v->decoded_bytes+=(uint64_t)used;
            v->at9_superframe_left=(uint32_t)used<=v->at9_superframe_left ? v->at9_superframe_left-(uint32_t)used : 0;
            if (++v->at9_frames==(uint32_t)v->at9_info.framesInSuperframe) {
                const uint32_t pad=v->at9_superframe_left<v->read_left ? v->at9_superframe_left : v->read_left;
                v->read+=pad; v->read_left-=pad;
                v->at9_frames=0; v->at9_superframe_left=(uint32_t)v->at9_info.superframeSize;
            }
            frames=(uint32_t)v->at9_info.frameSamples;
            const int decoded=v->at9_info.channels;
            for (uint32_t f=0;f<frames;++f) for (int c=0;c<channels;++c)
                v->pcm[f*(uint32_t)channels+(uint32_t)c]=c<decoded ? pcm[f*(uint32_t)decoded+(uint32_t)c]/32768.0f : 0.0f;
        } else {
            const int bytes=sample_bytes(v->format.type);
            const uint32_t stride=(uint32_t)(bytes*channels);
            frames=v->read_left/stride;
            if (frames>256) frames=256;
            if (!frames) { v->read_left=0; continue; }
            for (uint32_t f=0;f<frames;++f) for (int c=0;c<channels;++c) {
                const unsigned char *p=v->read+f*stride+(uint32_t)(c*bytes);
                float s;
                switch (v->format.type) {
                case WAVE_PCM_I8: s=(int8_t)p[0]/128.0f; break;
                case WAVE_PCM_U8: s=(p[0]-128)/128.0f; break;
                case WAVE_PCM_I16L: { int16_t x; memcpy(&x,p,2); s=x/32768.0f; break; }
                case WAVE_PCM_I16B: s=(int16_t)(p[0]<<8|p[1])/32768.0f; break;
                case WAVE_PCM_I24L: s=(int32_t)((uint32_t)p[0]<<8|(uint32_t)p[1]<<16|(uint32_t)p[2]<<24)/2147483648.0f; break;
                case WAVE_PCM_I32L: { int32_t x; memcpy(&x,p,4); s=x/2147483648.0f; break; }
                default: memcpy(&s,p,4); break;
                }
                v->pcm[f*(uint32_t)channels+(uint32_t)c]=s;
            }
            v->read+=frames*stride; v->read_left-=frames*stride; v->decoded_bytes+=frames*stride;
        }
        uint32_t start=0;
        if (v->skip_left) { start=v->skip_left<frames ? v->skip_left : frames; v->skip_left-=start; }
        uint32_t count=frames-start;
        if (count>v->block_left) count=v->block_left;
        if (v->block_left!=UINT32_MAX) v->block_left-=count;
        if (!count) continue;
        if (start) memmove(v->pcm,v->pcm+start*(uint32_t)channels,(size_t)count*(size_t)channels*sizeof(float));
        v->pcm_count=count; v->pcm_pos=0;
        v->decoded_samples+=count;
        return 1;
    }
    return 0;
}
static int next_frame(Voice *v, float *out) {
    while (v->pcm_pos>=v->pcm_count) if (!refill(v)) return 0;
    memcpy(out,v->pcm+v->pcm_pos*v->format.channels,v->format.channels*sizeof(float));
    ++v->pcm_pos;
    return 1;
}
static void envelope_restart(Voice *v) {
    v->env_point=0; v->env_pos=0; v->env_start=0.0f; v->releasing=0;
    v->env_level=v->env_active ? 0.0f : 1.0f;
}
/* Linear ramps from point to point (durations in samples); the last forward point holds until the
 * voice stops, then the release points run and the voice ends after them. */
static float envelope_step(Voice *v, int *finished) {
    if (!v->env_active) return 1.0f;
    const uint32_t first=v->releasing ? v->forward_points : 0;
    const uint32_t last=v->releasing ? v->forward_points+v->release_points : v->forward_points;
    if (v->env_point<first) { v->env_point=first; v->env_pos=0; v->env_start=v->env_level; }
    while (v->env_point<last) {
        const EnvPoint *p=&v->points[v->env_point];
        if (v->env_pos<p->duration) {
            v->env_level=v->env_start+(p->height-v->env_start)*(float)(v->env_pos+1)/(float)p->duration;
            ++v->env_pos;
            return v->env_level;
        }
        v->env_level=p->height; v->env_start=p->height; v->env_pos=0; ++v->env_point;
    }
    if (v->releasing) *finished=1;
    return v->env_level;
}
static void voice_end(Voice *v) {
    v->flags=0; v->ended=1;
    if (tracing()) printf("Ngs2 trace: voice %llx ended (%llu samples decoded)\n",(unsigned long long)v->handle,
                          (unsigned long long)v->decoded_samples);
}
/* One grain of a sampler voice at the system rate into out (frames x format channels). */
static void sampler_render(Voice *v, float *out, uint32_t frames, uint32_t rate) {
    const int channels=(int)v->format.channels;
    const double step=(double)v->format.rate*(double)v->pitch/(double)rate;
    for (uint32_t f=0;f<frames;++f) {
        while (v->frac>=1.0) {
            memcpy(v->prev,v->cur,sizeof(v->cur));
            if (!v->ended && !next_frame(v,v->cur)) { v->ended=1; memset(v->cur,0,sizeof(v->cur)); }
            v->frac-=1.0;
        }
        int finished=0;
        const float gain=envelope_step(v,&finished);
        for (int c=0;c<channels;++c)
            out[f*(uint32_t)channels+(uint32_t)c]=(v->prev[c]+(v->cur[c]-v->prev[c])*(float)v->frac)*gain;
        v->frac+=step;
        if (finished || (v->ended && v->frac>=1.0)) {
            memset(out+(f+1)*(uint32_t)channels,0,(size_t)(frames-f-1)*(size_t)channels*sizeof(float));
            voice_end(v);
            return;
        }
    }
}

/* ---- Routing ---- */
/* Level of input channel `in` into output channel `out` without a matrix: identity for equal
 * layouts, mono to the front pair, the PS4 8-channel order (FL FR C LFE SL SR BL BR) folded into
 * stereo. */
static float default_level(int in, int ins, int out, int outs) {
    if (ins==outs) return in==out ? 1.0f : 0.0f;
    if (ins==1) return out<2 ? 1.0f : 0.0f;
    if (outs==1) return 1.0f/(float)ins;
    if (outs==2) {
        if (in<2) return in==out ? 1.0f : 0.0f;
        if (in==2) return 0.7071f;                    /* centre */
        if (in==3) return 0.0f;                       /* LFE */
        return (in & 1)==out ? 0.7071f : 0.0f;        /* surround and back */
    }
    if (ins==2) return in==out ? 1.0f : 0.0f;
    return in==out && in<outs ? 1.0f : 0.0f;
}
/* Adds the voice's grain (frames x channels) to the inputs its ports are patched to. Matrices are
 * input-major: aLevel[in * outputs + out]. */
static void route(Voice *v, const float *grain, uint32_t frames, int channels) {
    for (uint32_t p=0;p<v->rack->max_ports;++p) {
        Voice *dest=v->ports[p].dest;
        if (!dest) continue;
        const int outs=dest->channels;
        float levels[MAX_CHANNELS][MAX_CHANNELS];
        const int32_t m=v->ports[p].matrix;
        const int matrix=m>=0 && (uint32_t)m<v->rack->max_matrices && v->matrix_levels[m]==(uint32_t)(channels*outs);
        for (int i=0;i<channels;++i) for (int o=0;o<outs;++o)
            levels[i][o]=(matrix ? v->matrix[m][i*outs+o] : default_level(i,channels,o,outs))*v->ports[p].volume;
        float *target=dest->input;
        for (uint32_t f=0;f<frames;++f) for (int o=0;o<outs;++o) {
            float sum=0.0f;
            for (int i=0;i<channels;++i) sum+=grain[f*(uint32_t)channels+(uint32_t)i]*levels[i][o];
            target[f*(uint32_t)outs+(uint32_t)o]+=sum;
        }
    }
}
static int mixer_depth(Voice *v, int limit) {
    if (v->rack->kind==KIND_MASTERING || !limit) return 0;
    int depth=0;
    for (uint32_t p=0;p<v->rack->max_ports;++p) if (v->ports[p].dest) {
        const int d=1+mixer_depth(v->ports[p].dest,limit-1);
        if (d>depth) depth=d;
    }
    return depth;
}
static int mixer_active(const Voice *v) { return (v->flags & STATE_IN_USE) && !(v->flags & STATE_PAUSED); }

/* Mixes one grain of every voice of the system: samplers, then mixer voices deepest first. */
static void system_grain(System *s, uint32_t frames) {
    for (Rack *r=s->racks;r;r=r->next) if (r->kind!=KIND_SAMPLER)
        for (uint32_t i=0;i<r->max_voices;++i) memset(r->voices[i].input,0,(size_t)frames*MAX_CHANNELS*sizeof(float));
    static float grain[MAX_GRAIN*MAX_CHANNELS]; /* under the lock: guest stacks can be 64 KiB */
    for (Rack *r=s->racks;r;r=r->next) {
        if (r->kind!=KIND_SAMPLER) continue;
        for (uint32_t i=0;i<r->max_voices;++i) {
            Voice *v=&r->voices[i];
            if (!(v->flags & STATE_PLAYING) || (v->flags & STATE_PAUSED)) continue;
            sampler_render(v,grain,frames,s->sample_rate);
            route(v,grain,frames,(int)v->format.channels);
        }
    }
    int deepest=0;
    for (Rack *r=s->racks;r;r=r->next) if (r->kind!=KIND_SAMPLER && r->kind!=KIND_MASTERING)
        for (uint32_t i=0;i<r->max_voices;++i) {
            Voice *v=&r->voices[i];
            v->depth=mixer_active(v) ? mixer_depth(v,16) : -1;
            if (v->depth>deepest) deepest=v->depth;
        }
    for (int depth=deepest;depth>=1;--depth)
        for (Rack *r=s->racks;r;r=r->next) if (r->kind!=KIND_SAMPLER && r->kind!=KIND_MASTERING)
            for (uint32_t i=0;i<r->max_voices;++i) {
                Voice *v=&r->voices[i];
                if (v->depth!=depth || r->kind==KIND_REVERB) continue;
                route(v,v->input,frames,v->channels);
            }
}
/* OrbisNgs2RenderBufferInfo: buffer, bufferSize, waveformType, numChannels. */
typedef struct { void *buffer; uint64_t size; uint32_t type, channels; } RenderBuffer;
static ABI int32_t system_render(Handle handle, const RenderBuffer *buffers, uint32_t count) {
    if (!buffers && count) return ERR_INVALID_BUFFER_ADDRESS;
    pthread_mutex_lock(&lock);
    System *s=find_system(handle);
    if (!s) { pthread_mutex_unlock(&lock); return ERR_INVALID_SYSTEM_HANDLE; }
    ++renders; ++s->renders;
    if (tracing() && s->renders==1)
        for (uint32_t b=0;b<count;++b)
            printf("Ngs2 trace: render buffer %u: %llu bytes, type 0x%x, %u channels\n",b,
                   (unsigned long long)buffers[b].size,buffers[b].type,buffers[b].channels);
    /* One grain per call: the buffers are the game's whole output buffers, it sends a grain of
     * them to sceAudioOutOutput after each render. */
    uint32_t total=s->grain;
    for (uint32_t b=0;b<count;++b) {
        const uint64_t frame=(uint64_t)(buffers[b].type==WAVE_PCM_I16L ? 2 : 4)*buffers[b].channels;
        if (frame && buffers[b].size/frame<total) total=(uint32_t)(buffers[b].size/frame);
    }
    for (uint32_t b=0;b<count;++b) {
        const uint64_t frame=(uint64_t)(buffers[b].type==WAVE_PCM_I16L ? 2 : 4)*buffers[b].channels;
        if (buffers[b].buffer) memset(buffers[b].buffer,0,total*frame);
    }
    for (uint32_t done=0;done<total;) {
        const uint32_t frames=total-done<MAX_GRAIN ? total-done : MAX_GRAIN;
        system_grain(s,frames);
        /* Each mastering voice is mixed into the buffer of its output, in that buffer's layout. */
        for (Rack *r=s->racks;r;r=r->next) {
            if (r->kind!=KIND_MASTERING) continue;
            for (uint32_t i=0;i<r->max_voices;++i) {
                Voice *v=&r->voices[i];
                if (!mixer_active(v)) continue;
                /* A peak limiter at full scale (the game turns the mastering limiter on): the
                 * gain drops at once on a peak and recovers over about 40 ms. */
                for (uint32_t f=0;f<frames;++f) {
                    float peak=0.0f;
                    for (int c=0;c<v->channels;++c) peak=fmaxf(peak,fabsf(v->input[f*(uint32_t)v->channels+(uint32_t)c]));
                    if (v->limiter<=0.0f) v->limiter=1.0f;
                    const float wanted=peak>1.0f ? 1.0f/peak : 1.0f;
                    v->limiter=wanted<v->limiter ? wanted : v->limiter+(1.0f-v->limiter)*0.0005f;
                    if (v->limiter<1.0f)
                        for (int c=0;c<v->channels;++c) v->input[f*(uint32_t)v->channels+(uint32_t)c]*=v->limiter;
                }
                for (uint32_t b=v->output;b<count && b==v->output;++b) {
                    const int outs=(int)buffers[b].channels;
                    if (!buffers[b].buffer || outs<1 || outs>MAX_CHANNELS) continue;
                    for (uint32_t f=0;f<frames;++f) for (int o=0;o<outs;++o) {
                        float sum=0.0f;
                        for (int c=0;c<v->channels;++c) sum+=v->input[f*(uint32_t)v->channels+(uint32_t)c]*default_level(c,v->channels,o,outs);
                        const size_t at=(size_t)(done+f)*(size_t)outs+(size_t)o;
                        if (buffers[b].type==WAVE_PCM_I16L) {
                            int16_t *pcm=buffers[b].buffer;
                            const float mixed=pcm[at]/32768.0f+sum;
                            pcm[at]=(int16_t)(mixed>=1.0f ? 32767 : mixed<=-1.0f ? -32768 : (int)lrintf(mixed*32767.0f));
                        } else ((float *)buffers[b].buffer)[at]+=sum;
                    }
                }
            }
        }
        done+=frames;
    }
    if (tracing()) {
        /* Every ~10 s: the loudest sample of each buffer and of each mastering voice's input since. */
        static float buffer_peak[8], voice_peak[8];
        for (uint32_t b=0;b<count && b<8;++b) {
            const uint64_t samples=(uint64_t)total*buffers[b].channels;
            for (uint64_t i=0;buffers[b].buffer && buffers[b].type==WAVE_PCM_F32L && i<samples;++i) {
                const float x=fabsf(((const float *)buffers[b].buffer)[i]);
                if (x>buffer_peak[b]) buffer_peak[b]=x;
            }
        }
        for (Rack *r=s->racks;r;r=r->next) if (r->kind==KIND_MASTERING)
            for (uint32_t i=0;i<r->max_voices && i<8;++i)
                for (uint32_t k=0;k<total*(uint32_t)r->voices[i].channels;++k)
                    if (fabsf(r->voices[i].input[k])>voice_peak[i]) voice_peak[i]=fabsf(r->voices[i].input[k]);
        if (s->renders%1875==0) {
            printf("Ngs2 trace: peaks: buffers");
            for (uint32_t b=0;b<count && b<8;++b) printf(" %.3f",buffer_peak[b]);
            printf("; mastering voices");
            for (int i=0;i<8;++i) printf(" %.3f",voice_peak[i]);
            putchar('\n');
            memset(buffer_peak,0,sizeof(buffer_peak)); memset(voice_peak,0,sizeof(voice_peak));
        }
    }
    pthread_mutex_unlock(&lock);
    return 0;
}

/* ---- Voice control ---- */
static void trace_param(const Voice *v, const ParamHeader *h) {
    if (!tracing()) return;
    printf("Ngs2 trace: voice %llx (%s %u) param 0x%08x size %u",(unsigned long long)v->handle,kind_name(v->rack->kind),
           v->index,h->id,h->size);
    const uint32_t *words=(const uint32_t *)(h+1);
    for (unsigned i=0;i<(h->size-8u)/4u && i<10;++i) printf(" %08x",words[i]);
    putchar('\n');
}
static void play(Voice *v) {
    v->flags=STATE_IN_USE|STATE_PLAYING;
    if (v->rack->kind==KIND_SAMPLER) {
        v->block=0; v->repeated=0; v->ended=0; v->exit_loop=0; v->frac=1.0;
        memset(v->prev,0,sizeof(v->prev)); memset(v->cur,0,sizeof(v->cur));
        envelope_restart(v);
        if (!v->block_count || !format_supported(&v->format)) { v->flags=STATE_EMPTY; return; }
        block_start(v);
        ++voices_played;
        if (tracing()) {
            const Voice *d=v->ports[0].dest;
            printf("Ngs2 trace: voice %llx plays %u blocks, first %u bytes at +%u (skip %u, %u samples), port 0 to %s %u (volume %.2f, matrix %d); data %p: %02x %02x %02x %02x, block: %02x %02x %02x %02x\n",
                   (unsigned long long)v->handle,v->block_count,v->blocks[0].size,v->blocks[0].offset,v->blocks[0].skip,
                   v->blocks[0].samples,d ? kind_name(d->rack->kind) : "nothing",d ? d->index : 0,v->ports[0].volume,
                   v->ports[0].matrix,(const void *)v->data,v->data[0],v->data[1],v->data[2],v->data[3],
                   v->data[v->blocks[0].offset],v->data[v->blocks[0].offset+1],v->data[v->blocks[0].offset+2],
                   v->data[v->blocks[0].offset+3]);
        }
    }
}
static int32_t voice_event(Voice *v, uint32_t event) {
    switch (event) {
    case EVENT_PLAY: play(v); return 0;
    case EVENT_STOP:
        if (v->rack->kind==KIND_SAMPLER && v->env_active && v->release_points && (v->flags & STATE_PLAYING)) {
            if (!v->releasing) { v->releasing=1; v->env_point=0; }
        } else v->flags=0;
        return 0;
    case EVENT_STOP_IMMEDIATE: case EVENT_KILL: v->flags=0; return 0;
    case EVENT_PAUSE: if (v->flags & STATE_PLAYING) v->flags|=STATE_PAUSED; return 0;
    case EVENT_RESUME: v->flags&=~(uint32_t)STATE_PAUSED; return 0;
    default: return ERR_INVALID_EVENT_TYPE;
    }
}
static int32_t common_param(Voice *v, const ParamHeader *h) {
    const unsigned char *p=(const unsigned char *)h;
    switch (h->id) {
    case PARAM_MATRIX_LEVELS: {
        const uint32_t id=read32(p,8), levels=read32(p,12);
        const float *values; memcpy(&values,p+16,8);
        if (id>=v->rack->max_matrices) return ERR_INVALID_MATRIX_INDEX;
        const uint32_t n=levels>MAX_CHANNELS*MAX_CHANNELS ? MAX_CHANNELS*MAX_CHANNELS : levels;
        if (values) memcpy(v->matrix[id],values,n*sizeof(float));
        v->matrix_levels[id]=values ? n : 0;
        return 0;
    }
    case PARAM_PORT_MATRIX: {
        const uint32_t port=read32(p,8); int32_t id; memcpy(&id,p+12,4);
        if (port>=v->rack->max_ports) return ERR_INVALID_PORT_INDEX;
        v->ports[port].matrix=id; return 0;
    }
    case PARAM_PORT_VOLUME: {
        const uint32_t port=read32(p,8); float level; memcpy(&level,p+12,4);
        if (port>=v->rack->max_ports) return ERR_INVALID_PORT_INDEX;
        v->ports[port].volume=level; return 0;
    }
    case PARAM_PORT_DELAY: return 0; /* delays of a few samples between buses are not reproduced */
    case PARAM_PATCH: {
        const uint32_t port=read32(p,8), input=read32(p,12); Handle dest; memcpy(&dest,p+16,8);
        if (port>=v->rack->max_ports) return ERR_INVALID_PORT_INDEX;
        Voice *d=dest ? find_voice(dest) : NULL;
        if (dest && !d) return ERR_INVALID_VOICE_HANDLE;
        v->ports[port].dest=d && d->rack->kind!=KIND_SAMPLER ? d : NULL;
        v->ports[port].input=input;
        return 0;
    }
    case PARAM_EVENT: return voice_event(v,read32(p,8));
    case PARAM_CALLBACK:
        memcpy(&v->callback,p+8,8); memcpy(&v->callback_data,p+16,8); v->callback_flags=read32(p,24);
        return 0;
    default: return 1;
    }
}
static int32_t sampler_param(Voice *v, const ParamHeader *h) {
    const unsigned char *p=(const unsigned char *)h;
    switch (h->id & 0xffff) {
    case SAMPLER_SETUP:
        memcpy(&v->format,p+8,sizeof(v->format));
        if (!format_supported(&v->format)) {
            printf("Runtime: Ngs2 waveform type 0x%x, %u channels, %u Hz is not supported\n",v->format.type,
                   v->format.channels,v->format.rate);
            return ERR_INVALID_WAVEFORM_FORMAT;
        }
        v->channels=(int)v->format.channels;
        v->flags|=STATE_IN_USE;
        v->block_count=0; v->data=NULL;
        return 0;
    case SAMPLER_WAVEFORM_BLOCKS: {
        const unsigned char *data; memcpy(&data,p+8,8);
        const uint32_t flags=read32(p,16), count=read32(p,20);
        const WaveBlock *blocks; memcpy(&blocks,p+24,8);
        if (!blocks && count) blocks=(const WaveBlock *)(p+32);
        (void)flags;
        v->data=data;
        v->block_count=count<MAX_BLOCKS ? count : MAX_BLOCKS;
        if (blocks) memcpy(v->blocks,blocks,v->block_count*sizeof(WaveBlock));
        return 0;
    }
    case SAMPLER_WAVEFORM_ADDRESS: {
        const unsigned char *from, *to; memcpy(&from,p+8,8); memcpy(&to,p+16,8);
        if (v->data==from) { if (v->read) v->read=to+(v->read-from); v->data=to; }
        return 0;
    }
    case SAMPLER_WAVEFORM_FRAME_OFFSET: return 0;
    case SAMPLER_EXIT_LOOP: v->exit_loop=1; return 0;
    case SAMPLER_PITCH: {
        float ratio; memcpy(&ratio,p+8,4);
        v->pitch=ratio>0.0f && ratio<16.0f ? ratio : 1.0f;
        return 0;
    }
    case SAMPLER_ENVELOPE: {
        const uint32_t forward=read32(p,8), release=read32(p,12);
        const EnvPoint *points; memcpy(&points,p+16,8);
        const uint32_t n=forward+release;
        if (!points || !n || n>MAX_POINTS) { v->env_active=0; v->forward_points=v->release_points=0; return 0; }
        memcpy(v->points,points,n*sizeof(EnvPoint));
        v->forward_points=forward; v->release_points=release; v->env_active=1;
        return 0;
    }
    default: return 1; /* filters, distortion, user effects, peak meters: the sound passes unchanged */
    }
}
/* Setup: the channel count first (a custom rack's setup gives its input channels first). */
static int32_t mixer_param(Voice *v, const ParamHeader *h) {
    if ((h->id & 0xffff)==MIXER_SETUP && h->id>>16==v->rack->id) {
        const uint32_t channels=read32(h,8);
        v->channels=channels>=1 && channels<=MAX_CHANNELS ? (int)channels : 2;
        v->flags|=STATE_IN_USE;
        return 0;
    }
    if (v->rack->kind==KIND_MASTERING && h->id==((uint32_t)RACK_MASTERING<<16|MASTERING_OUTPUT)) {
        v->output=read32(h,8);
        return 0;
    }
    return 1; /* envelopes, compressors, limiters, filters, reverb settings: passed through */
}
static ABI int32_t voice_control(Handle handle, const ParamHeader *params) {
    pthread_mutex_lock(&lock);
    Voice *v=find_voice(handle);
    if (!v) { pthread_mutex_unlock(&lock); return ERR_INVALID_VOICE_HANDLE; }
    int32_t result=0;
    unsigned walked=0;
    for (const ParamHeader *h=params; h && walked<256; h=h->next ? (const ParamHeader *)((const char *)h+h->next) : NULL, ++walked) {
        trace_param(v,h);
        int32_t r=common_param(v,h);
        if (r==1) r=v->rack->kind==KIND_SAMPLER ? sampler_param(v,h) : mixer_param(v,h);
        if (r==1) {
            r=0;
            static uint32_t reported[64]; static unsigned reported_count;
            unsigned k=0;
            while (k<reported_count && reported[k]!=h->id) ++k;
            if (k==reported_count && reported_count<64) {
                reported[reported_count++]=h->id;
                printf("Runtime: Ngs2 %s parameter 0x%08x has no effect here\n",kind_name(v->rack->kind),h->id);
            }
        }
        if (r && !result) result=r;
        if (!h->next) break;
    }
    pthread_mutex_unlock(&lock);
    return result;
}
/* OrbisNgs2SamplerVoiceState: stateFlags, envelopeHeight, peakHeight, reserved, numDecodedSamples
 * @16, decodedDataSize @24, userData @32, waveformData @40. Other voices: stateFlags first. */
static ABI int32_t voice_state(Handle handle, unsigned char *state, uint64_t size) {
    if (!state || size<4) return ERR_INVALID_OUT_ADDRESS;
    pthread_mutex_lock(&lock);
    Voice *v=find_voice(handle);
    if (!v) { pthread_mutex_unlock(&lock); return ERR_INVALID_VOICE_HANDLE; }
    memset(state,0,size>256 ? 256 : size);
    memcpy(state,&v->flags,4);
    if (size>=8) memcpy(state+4,&v->env_level,4);
    if (v->rack->kind==KIND_SAMPLER && size>=48) {
        memcpy(state+16,&v->decoded_samples,8);
        memcpy(state+24,&v->decoded_bytes,8);
        const uint64_t user=v->block<v->block_count ? v->blocks[v->block].user : 0;
        memcpy(state+32,&user,8);
        memcpy(state+40,&v->data,8);
    }
    pthread_mutex_unlock(&lock);
    return 0;
}

/* ---- Waveform parsing (RIFF WAVE with PCM or ATRAC9) ---- */
/* OrbisNgs2WaveformInfo (200 bytes): format @0, dataOffset @24, dataSize @28, loopBegin @32,
 * loopEnd @36, numSamples @40, audioUnitSize @44, numAudioUnitSamples @48, numAudioUnitPerFrame
 * @52, audioFrameSize @56, numAudioFrameSamples @60, numDelaySamples @64, numBlocks @68,
 * aBlock[4] @72. Block offsets count from the start of the RIFF data. */
typedef struct {
    WaveFormat format;
    uint32_t data_offset, data_size, loop_begin, loop_end, samples, unit_size, unit_samples, units_per_frame,
             frame_size, frame_samples, delay, block_count;
    WaveBlock blocks[4];
} WaveInfo;
_Static_assert(sizeof(WaveInfo)==200,"OrbisNgs2WaveformInfo layout");
/* The block of samples [from, to) of the waveform. A block that starts after the first unit also
 * decodes the unit before (and skips its samples): ATRAC9 frames overlap, so a decoder started
 * cold would play the first frame wrong at every loop. */
static WaveBlock make_block(const WaveInfo *w, uint32_t from, uint32_t to, uint32_t repeats) {
    WaveBlock b={0};
    const uint32_t unit=w->unit_samples ? w->unit_samples : 1;
    uint32_t first=(w->delay+from)/unit;
    uint32_t skip=(w->delay+from)%unit;
    if (w->format.type==WAVE_ATRAC9 && first) { --first; skip+=unit; }
    const uint32_t end=(w->delay+to+unit-1)/unit;
    b.offset=w->data_offset+first*w->unit_size;
    b.size=(end-first)*w->unit_size;
    if (b.offset+b.size>w->data_offset+w->data_size) b.size=w->data_offset+w->data_size-b.offset;
    b.skip=skip; b.samples=to-from; b.repeats=repeats;
    return b;
}
static int32_t parse_waveform(const unsigned char *data, uint64_t size, WaveInfo *out) {
    if (size<12 || memcmp(data,"RIFF",4) || memcmp(data+8,"WAVE",4)) return ERR_INVALID_WAVEFORM_DATA;
    WaveInfo w; memset(&w,0,sizeof(w));
    int have_format=0, have_data=0, have_loop=0;
    uint32_t total=0;
    uint64_t at=12;
    while (at+8<=size) {
        const uint32_t tag=read32(data,at), length=read32(data,at+4);
        const unsigned char *body=data+at+8;
        const uint64_t avail=size-(at+8);
        if (tag==0x20746d66 && length>=16 && avail>=16) {                /* "fmt " */
            uint16_t format_tag; memcpy(&format_tag,body,2);
            uint16_t channels; memcpy(&channels,body+2,2);
            uint16_t align; memcpy(&align,body+12,2);
            uint16_t bits; memcpy(&bits,body+14,2);
            w.format.channels=channels; w.format.rate=read32(body,4);
            w.unit_size=align;
            if (format_tag==0xfffe && length>=48 && avail>=48 && body[24]==0xd2 && body[25]==0x42) {
                w.format.type=WAVE_ATRAC9;                               /* the ATRAC9 subformat GUID */
                memcpy(&w.format.config,body+44,4);
                void *decoder=Atrac9GetHandle();
                Atrac9CodecInfo info;
                if (!decoder || Atrac9InitDecoder(decoder,(unsigned char *)body+44) || Atrac9GetCodecInfo(decoder,&info)) {
                    if (decoder) Atrac9ReleaseHandle(decoder);
                    return ERR_INVALID_WAVEFORM_FORMAT;
                }
                Atrac9ReleaseHandle(decoder);
                w.unit_size=(uint32_t)info.superframeSize;
                w.frame_samples=(uint32_t)info.frameSamples;
                w.units_per_frame=1;
                w.unit_samples=(uint32_t)(info.frameSamples*info.framesInSuperframe);
                w.frame_size=(uint32_t)info.superframeSize/(uint32_t)info.framesInSuperframe;
            } else {
                const uint16_t base=format_tag==0xfffe && length>=26 ? (uint16_t)(body[24]|body[25]<<8) : format_tag;
                w.format.type=base==3 && bits==32 ? WAVE_PCM_F32L : base==1 && bits==16 ? WAVE_PCM_I16L :
                              base==1 && bits==8 ? WAVE_PCM_U8 : base==1 && bits==24 ? WAVE_PCM_I24L :
                              base==1 && bits==32 ? WAVE_PCM_I32L : 0;
                if (!w.format.type) return ERR_INVALID_WAVEFORM_TYPE;
                w.unit_samples=1; w.frame_samples=1; w.frame_size=align; w.units_per_frame=1;
            }
            have_format=1;
        } else if (tag==0x74636166 && length>=8 && avail>=8) {           /* "fact": samples, encoder delay */
            total=read32(body,0);
            if (length>=12) w.delay=read32(body,4);
        } else if (tag==0x6c706d73 && length>=36 && avail>=36) {          /* "smpl": the first loop */
            if (read32(body,28) && length>=60 && avail>=60) {
                w.loop_begin=read32(body,36+8); w.loop_end=read32(body,36+12);
                have_loop=w.loop_end>=w.loop_begin;
            }
        } else if (tag==0x61746164) {                                     /* "data" */
            w.data_offset=(uint32_t)(at+8);
            w.data_size=(uint32_t)(length<=avail ? length : avail);
            have_data=1;
        }
        at+=8+(uint64_t)length+(length&1);
    }
    if (!have_format || !have_data || !w.unit_size) return ERR_INVALID_WAVEFORM_DATA;
    if (w.format.type!=WAVE_ATRAC9) total=w.data_size/w.unit_size;
    else if (!total) total=w.data_size/w.unit_size*w.unit_samples-w.delay;
    w.samples=total;
    if (have_loop && w.loop_end>=total) w.loop_end=total ? total-1 : 0;
    if (!have_loop) { w.loop_begin=0; w.loop_end=total ? total-1 : 0; }
    if (have_loop && w.loop_begin<w.loop_end) {
        if (w.loop_begin) w.blocks[w.block_count++]=make_block(&w,0,w.loop_begin,0);
        w.blocks[w.block_count++]=make_block(&w,w.loop_begin,w.loop_end+1,INFINITE_REPEATS);
        if (w.loop_end+1<total) w.blocks[w.block_count++]=make_block(&w,w.loop_end+1,total,0);
    } else w.blocks[w.block_count++]=make_block(&w,0,total,0);
    /* The first block plays the encoder delay away from the very start, without a primer. */
    if (w.format.type==WAVE_ATRAC9 && w.blocks[0].offset==w.data_offset) w.blocks[0].skip=w.delay;
    *out=w;
    if (tracing())
        printf("Ngs2 trace: waveform type 0x%x %u ch %u Hz, %u samples, delay %u, loop %u-%u, %u blocks\n",
               w.format.type,w.format.channels,w.format.rate,w.samples,w.delay,w.loop_begin,w.loop_end,w.block_count);
    return 0;
}
static ABI int32_t parse_data(const unsigned char *data, uint64_t size, WaveInfo *out) {
    if (!data || !out) return ERR_INVALID_OUT_ADDRESS;
    return parse_waveform(data,size,out);
}
/* The header of a waveform file (from `offset`); only the first 64 KiB are read for the chunks
 * before the samples. */
static ABI int32_t parse_file(const char *path, uint64_t offset, WaveInfo *out) {
    if (!path || !out) return ERR_INVALID_OUT_ADDRESS;
    int64_t fd=runtime_file_open(path,0,0);
    if (fd<0) return ERR_INVALID_WAVEFORM_DATA;
    enum { HEAD=64*1024 };
    unsigned char *head=malloc(HEAD);
    int64_t got=head ? runtime_file_pread((int)fd,head,HEAD,(int64_t)offset) : -1;
    runtime_file_close((int)fd);
    if (got<12) { free(head); return ERR_INVALID_WAVEFORM_DATA; }
    /* The data chunk's size may reach past what was read: parse with the size it declares. */
    const int32_t result=parse_waveform(head,(uint64_t)got,out);
    if (!result) {
        uint64_t at=12;
        while (at+8<=(uint64_t)got) {
            const uint32_t tag=read32(head,at), length=read32(head,at+4);
            if (tag==0x61746164) { out->data_size=length; break; }
            at+=8+(uint64_t)length+(length&1);
        }
    }
    free(head);
    return result;
}

static const RuntimeExport exports[]={
    {"sceNgs2SystemQueryBufferSize",system_query_size}, {"sceNgs2SystemCreate",system_create},
    {"sceNgs2SystemDestroy",system_destroy}, {"sceNgs2SystemRender",system_render},
    {"sceNgs2RackQueryBufferSize",rack_query_size}, {"sceNgs2RackCreate",rack_create},
    {"sceNgs2RackDestroy",rack_destroy}, {"sceNgs2RackGetInfo",rack_info}, {"sceNgs2RackGetVoiceHandle",rack_voice},
    {"sceNgs2VoiceControl",voice_control}, {"sceNgs2VoiceGetState",voice_state},
    {"sceNgs2ParseWaveformData",parse_data}, {"sceNgs2ParseWaveformFile",parse_file},
};
uintptr_t runtime_ngs2_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_ngs2_report(void) {
    printf("Runtime: Ngs2 renders=%zu, voices played=%zu, blocks finished=%zu, decode errors=%zu\n",renders,
           voices_played,blocks_finished,decode_errors);
}
