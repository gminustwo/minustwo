/* libSceNgs2 (src/runtime_ngs2.c): the waveform parser's blocks and a sampler voice mixed through
 * a mastering voice into a render buffer, as GODZILLA drives them. */
#undef NDEBUG /* the checks are asserts; release builds define NDEBUG */
#include "../src/runtime_ngs2.c"
#include <assert.h>

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) { (void)table; (void)count; (void)nid; return 0; }
int64_t runtime_file_open(const char *path, int flags, int mode) { (void)path; (void)flags; (void)mode; return -1; }
int64_t runtime_file_pread(int fd, void *buffer, uint64_t size, int64_t offset) { (void)fd; (void)buffer; (void)size; (void)offset; return -1; }
int64_t runtime_file_close(int fd) { (void)fd; return 0; }

static void put32(unsigned char *p, uint32_t v) { memcpy(p,&v,4); }
static void put16(unsigned char *p, uint16_t v) { memcpy(p,&v,2); }

/* A mono 16-bit 48 kHz RIFF with `samples` samples and, when loop_end, a smpl loop. */
static size_t make_wave(unsigned char *out, const int16_t *pcm, uint32_t samples, uint32_t loop_begin, uint32_t loop_end) {
    unsigned char *p=out;
    memcpy(p,"RIFF",4); memcpy(p+8,"WAVE",4); p+=12;
    memcpy(p,"fmt ",4); put32(p+4,16); put16(p+8,1); put16(p+10,1); put32(p+12,48000); put32(p+16,96000);
    put16(p+20,2); put16(p+22,16); p+=24;
    if (loop_end) {
        memcpy(p,"smpl",4); put32(p+4,60); memset(p+8,0,60); put32(p+8+28,1); put32(p+8+36+8,loop_begin); put32(p+8+36+12,loop_end);
        p+=68;
    }
    memcpy(p,"data",4); put32(p+4,samples*2); memcpy(p+8,pcm,samples*2); p+=8+samples*2;
    put32(out+4,(uint32_t)(p-out-8));
    return (size_t)(p-out);
}

static void param(Handle voice, const void *block) { assert(voice_control(voice,block)==0); }

int main(void) {
    static int16_t pcm[1000];
    for (int i=0;i<1000;++i) pcm[i]=(int16_t)(i*16);
    static unsigned char wave[4096];

    /* Parsing: no loop is one block; a loop is intro, endless loop and tail, in samples of the data. */
    WaveInfo info;
    size_t size=make_wave(wave,pcm,1000,0,0);
    assert(parse_data(wave,size,&info)==0);
    assert(info.format.type==WAVE_PCM_I16L && info.format.channels==1 && info.format.rate==48000);
    assert(info.samples==1000 && info.block_count==1 && info.blocks[0].offset==info.data_offset && info.blocks[0].samples==1000);
    size=make_wave(wave,pcm,1000,100,599);
    assert(parse_data(wave,size,&info)==0 && info.block_count==3);
    assert(info.loop_begin==100 && info.loop_end==599);
    assert(info.blocks[0].samples==100 && info.blocks[0].repeats==0);
    assert(info.blocks[1].offset==info.data_offset+200 && info.blocks[1].samples==500 && info.blocks[1].repeats==INFINITE_REPEATS);
    assert(info.blocks[2].offset==info.data_offset+1200 && info.blocks[2].samples==400);
    assert(parse_data((const unsigned char *)"RIFF\0\0\0\0WAVX",12,&info)==ERR_INVALID_WAVEFORM_DATA);

    /* A system with a sampler rack (default options: SAMPLER_DEFAULT_VOICES voices) and a stereo
     * mastering voice. */
    BufferInfo buffer={0};
    Handle system=0, sampler_rack=0, mastering_rack=0, sampler=0, master=0;
    assert(system_query_size(NULL,&buffer)==0 && buffer.size);
    assert(system_create(NULL,&buffer,&system)==0);
    assert(rack_create(system,RACK_SAMPLER,NULL,&buffer,&sampler_rack)==0);
    unsigned char option[136]={0};
    put32(option,sizeof(option)); put32(option+32,1);
    assert(rack_create(system,RACK_MASTERING,option,&buffer,&mastering_rack)==0);
    assert(rack_create(system,0x9999,NULL,&buffer,&sampler_rack)==ERR_INVALID_RACK_ID);
    assert(rack_voice(sampler_rack,SAMPLER_DEFAULT_VOICES-1,&sampler)==0 &&
           rack_voice(sampler_rack,SAMPLER_DEFAULT_VOICES,&sampler)==ERR_INVALID_VOICE_INDEX);
    assert(rack_voice(sampler_rack,0,&sampler)==0 && rack_voice(mastering_rack,0,&master)==0);

    struct { ParamHeader h; uint32_t a, b; } setup={{16,0,0x30000000},2,0}, output={{16,0,0x30000005},0,0};
    struct { ParamHeader h; uint32_t event; } play_event={{12,0,PARAM_EVENT},EVENT_PLAY};
    param(master,&setup); param(master,&output); param(master,&play_event);

    size=make_wave(wave,pcm,1000,0,0);
    assert(parse_data(wave,size,&info)==0);
    struct { ParamHeader h; WaveFormat format; uint32_t flags, reserved; } sampler_setup={{40,0,0x10000000},info.format,0,0};
    struct { ParamHeader h; const void *data; uint32_t flags, count; const WaveBlock *blocks; } blocks=
        {{32,0,0x10000001},wave,0,info.block_count,info.blocks};
    struct { ParamHeader h; uint32_t port, input; Handle dest; } patch={{24,0,PARAM_PATCH},0,0,master};
    param(sampler,&sampler_setup); param(sampler,&blocks); param(sampler,&patch); param(sampler,&play_event);

    /* One render is one grain: mono to both channels, a sample late (the resampler's first step). */
    static float out[256*2];
    RenderBuffer render={out,sizeof(out),WAVE_PCM_F32L,2};
    assert(system_render(system,&render,1)==0);
    for (int f=1;f<256;++f) assert(out[2*f]==pcm[f-1]/32768.0f && out[2*f+1]==out[2*f]);

    /* Port volume (parameter 2) scales what the port sends. */
    struct { ParamHeader h; uint32_t port; float level; } volume={{16,0,PARAM_PORT_VOLUME},0,0.5f};
    param(sampler,&volume);
    assert(system_render(system,&render,1)==0);
    assert(fabsf(out[2]-0.5f*pcm[256]/32768.0f)<1e-6f);

    /* When the waveform has played the voice is free again. */
    unsigned char state[48];
    assert(voice_state(sampler,state,sizeof(state))==0 && (state[0] & STATE_PLAYING));
    for (int i=0;i<4;++i) assert(system_render(system,&render,1)==0);
    assert(voice_state(sampler,state,sizeof(state))==0 && state[0]==0);
    uint64_t decoded; memcpy(&decoded,state+16,8);
    assert(decoded==1000);

    /* A stream (GODZILLA's voice lines): played and paused before its data, then its ring buffer's
     * first part (blocks flag 1, the whole stream's sample count); it waits empty when that part is
     * played, and the part the game appends (flag 3, its own data address, the same skip and count
     * again) goes on with it until the stream's count. */
    Handle stream=0;
    assert(rack_voice(sampler_rack,1,&stream)==0);
    struct { ParamHeader h; uint32_t event; } pause_event={{12,0,PARAM_EVENT},EVENT_PAUSE},
        resume_event={{12,0,PARAM_EVENT},EVENT_RESUME};
    param(stream,&sampler_setup); param(stream,&patch); param(stream,&play_event); param(stream,&pause_event);
    assert(voice_state(stream,state,sizeof(state))==0 &&
           state[0]==(STATE_IN_USE|STATE_PLAYING|STATE_PAUSED|STATE_EMPTY));
    WaveBlock part={0,200*2,0,0,600,0,0};
    blocks.data=pcm; blocks.flags=BLOCKS_STREAM; blocks.count=1; blocks.blocks=&part;
    param(stream,&blocks); param(stream,&resume_event);
    assert(voice_state(stream,state,sizeof(state))==0 && state[0]==(STATE_IN_USE|STATE_PLAYING));
    assert(system_render(system,&render,1)==0);
    /* (The resampler needs the next sample for a frame: the wait starts one frame early, and the
     * first frame after it is the sample it held back.) */
    for (int f=1;f<200;++f) assert(out[2*f]==pcm[f-1]/32768.0f);
    for (int f=200;f<256;++f) assert(out[2*f]==0.0f);
    assert(voice_state(stream,state,sizeof(state))==0 && (state[0] & STATE_EMPTY) && (state[0] & STATE_PLAYING));
    WaveBlock next={0,400*2,0,256,600,0,0};
    blocks.data=pcm+200; blocks.flags=BLOCKS_STREAM|BLOCKS_APPEND; blocks.blocks=&next;
    param(stream,&blocks);
    assert(voice_state(stream,state,sizeof(state))==0 && !(state[0] & STATE_EMPTY));
    assert(system_render(system,&render,1)==0);
    for (int f=0;f<256;++f) assert(out[2*f]==pcm[199+f]/32768.0f);
    for (int i=0;i<2;++i) assert(system_render(system,&render,1)==0);
    assert(voice_state(stream,state,sizeof(state))==0 && state[0]==0);
    memcpy(&decoded,state+16,8);
    assert(decoded==600);

    /* A voice played again reports what it decoded since: a stream's reader refills its ring by the
     * decoded size, and the sizes of earlier sounds made it overwrite what was still to play. */
    WaveBlock whole={0,1000*2,0,0,1000,0,0};
    blocks.data=pcm; blocks.flags=0; blocks.blocks=&whole;
    param(stream,&blocks); param(stream,&play_event);
    assert(system_render(system,&render,1)==0);
    assert(voice_state(stream,state,sizeof(state))==0);
    uint64_t decoded_bytes;
    memcpy(&decoded,state+16,8); memcpy(&decoded_bytes,state+24,8);
    assert(decoded==256 && decoded_bytes==512);

    /* Destroying hands the game's buffers back. */
    BufferInfo back={0};
    assert(rack_destroy(sampler_rack,&back)==0 && system_destroy(system,&back)==0);
    assert(system_render(system,&render,1)==ERR_INVALID_SYSTEM_HANDLE);
    puts("ngs2 test: ok");
    return 0;
}
