/* Exercise actual guest file operations on a linked overlay and writable saves. */
#include "../src/runtime_file.c"
#include <assert.h>
static int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }
int32_t runtime_guest_errno(int e) { return e; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t n,const char *name) {
    for (size_t i=0;i<n;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
static int japanese_voices;
int mtgpu_japanese_voices(void) { return japanese_voices; }
static unsigned char save_block[SAVE_SIZE];
static int save_block_found=1;
unsigned char *runtime_save_block(void) { return save_block_found ? save_block : NULL; }
void runtime_state_file(const char *guest) { (void)guest; } /* runtime_state.c: tested by test_state.c */
static void write_file(const char *dir,const char *name,const char *text) {
    char path[600]; snprintf(path,sizeof(path),"%s/%s",dir,name);
    FILE *f=fopen(path,"w"); assert(f); assert(fputs(text,f)>=0); assert(!fclose(f));
}
static void remove_file(const char *dir,const char *name) {
    char path[600]; snprintf(path,sizeof(path),"%s/%s",dir,name);
    assert(!unlink(path));
}
/* What the game reads from guest path `path`, with its stat size. */
static void expect(const char *path,const char *text) {
    int fd=(int)do_open(path,0,0); assert(fd>=3);
    char content[32]={0}; assert(do_read(fd,content,sizeof(content)-1)==(int64_t)strlen(text) && !strcmp(content,text));
    assert(!do_close(fd));
    GuestStat info; assert(!do_stat(path,&info) && info.size==(int64_t)strlen(text));
}
/* Japanese voices: VOICE_*_EN.at9 under sound/ opens the Japanese recording when there is one. */
static void japanese_voice_files(const char *game) {
    char sound[520],st[530];
    snprintf(sound,sizeof(sound),"%.511s/sound",game);
    snprintf(st,sizeof(st),"%s/st",sound);
    assert(!mkdir(sound,0755) && !mkdir(st,0755));
    write_file(st,"VOICE_TUT_01_00.at9","japanese");
    write_file(st,"VOICE_TUT_01_00_EN.at9","english line");
    write_file(st,"VOICE_ONLY_EN.at9","english only");
    write_file(st,"SE_ROAR_EN.at9","effect en");
    write_file(st,"SE_ROAR.at9","effect");
    write_file(game,"VOICE_TOP_EN.at9","not sound");
    write_file(game,"VOICE_TOP.at9","top");
    japanese_voices=0;
    expect("sound/st/VOICE_TUT_01_00_EN.at9","english line");
    japanese_voices=1;
    expect("sound/st/VOICE_TUT_01_00_EN.at9","japanese");
    expect("/app0/sound/st/VOICE_TUT_01_00_EN.at9","japanese");
    expect("sound/st/VOICE_TUT_01_00.at9","japanese");
    expect("sound/st/VOICE_ONLY_EN.at9","english only");  /* no Japanese recording: the English one */
    expect("sound/st/SE_ROAR_EN.at9","effect en");        /* not a voice line */
    expect("VOICE_TOP_EN.at9","not sound");               /* not under sound/ */
    japanese_voices=0;
    expect("sound/st/VOICE_TUT_01_00_EN.at9","english line");
    remove_file(st,"VOICE_TUT_01_00.at9"); remove_file(st,"VOICE_TUT_01_00_EN.at9"); remove_file(st,"VOICE_ONLY_EN.at9");
    remove_file(st,"SE_ROAR_EN.at9"); remove_file(st,"SE_ROAR.at9");
    remove_file(game,"VOICE_TOP_EN.at9"); remove_file(game,"VOICE_TOP.at9");
    assert(!rmdir(st) && !rmdir(sound));
}
/* A GODZILLA save (DATA.BIN): header 1, `version` (1 from 01.00, 2 from 01.01), size; language word
 * `language`; the game's checksum at +0 unless `broken`. */
static void write_save(const char *dir,uint16_t language,int broken,uint32_t version) {
    static unsigned char data[SAVE_SIZE];
    memset(data,0,sizeof(data));
    const uint32_t header[3]={1,version,SAVE_SIZE};
    memcpy(data+4,header,sizeof(header));
    for (size_t i=0x100;i<SAVE_SIZE;i+=97) data[i]=(unsigned char)(i*7);  /* bytes of both signs */
    memcpy(data+SAVE_LANGUAGE,&language,2);
    int32_t sum=broken;
    for (size_t i=4;i<SAVE_SIZE;++i) sum+=(signed char)data[i];
    memcpy(data,&sum,4);
    char path[600]; snprintf(path,sizeof(path),"%s/DATA.BIN",dir);
    FILE *f=fopen(path,"wb"); assert(f); assert(fwrite(data,1,SAVE_SIZE,f)==SAVE_SIZE); assert(!fclose(f));
}
/* What the game reads from /savedata0/DATA.BIN: whole (in two reads) and by pread; returns its language word
 * after checking that the game's checksum holds. */
static uint16_t read_save(int flags) {
    static unsigned char data[SAVE_SIZE];
    int fd=(int)do_open("/savedata0/DATA.BIN",flags,0); assert(fd>=3);
    assert(do_read(fd,data,0x2b)==0x2b);  /* ends inside the language word */
    assert(do_read(fd,data+0x2b,SAVE_SIZE-0x2b)==SAVE_SIZE-0x2b);
    unsigned char part[4];
    assert(do_pread(fd,part,4,0x29)==4 && !memcmp(part,data+0x29,4));
    assert(do_pread(fd,part,2,0)==2 && !memcmp(part,data,2));
    assert(!do_close(fd));
    int32_t sum=0, stored;
    for (size_t i=4;i<SAVE_SIZE;++i) sum+=(signed char)data[i];
    memcpy(&stored,data,4);
    assert(sum==stored);
    uint16_t language; memcpy(&language,data+SAVE_LANGUAGE,2);
    return language;
}
/* The save's language: DATA.BIN is read with the one the game plays in (the save block's word). */
static void save_language(const char *root) {
    char dir[560]; snprintf(dir,sizeof(dir),"%s/savedata",root);
    assert(!mkdir(dir,0755));
    assert(!runtime_file_mount("/savedata0",dir));
    const uint16_t italian=5, german=3;
    memcpy(save_block+SAVE_LANGUAGE,&italian,2);
    write_save(dir,german,0,1);
    assert(read_save(0)==italian);
    write_save(dir,german,0,2);            /* a save of 01.01 */
    assert(read_save(0)==italian);
    write_save(dir,german,0,3);            /* a version no build of the game reads */
    assert(read_save(0)==german);
    write_save(dir,german,0,1);
    assert(read_save(2)==german);          /* opened to write: the file's own bytes */
    setenv("MT_SAVE_LANGUAGE","0",1);
    assert(read_save(0)==german);
    unsetenv("MT_SAVE_LANGUAGE");
    save_block_found=0;
    assert(read_save(0)==german);          /* no save block in this game build */
    save_block_found=1;
    write_save(dir,italian,0,1);
    assert(read_save(0)==italian);         /* already the game's */
    write_save(dir,german,1,1);            /* a broken checksum stays the game's to find */
    int fd=(int)do_open("/savedata0/DATA.BIN",0,0); assert(fd>=3);
    uint16_t language; assert(do_pread(fd,&language,2,SAVE_LANGUAGE)==2 && language==german);
    assert(!do_close(fd));
    assert(language_patches==0);
    runtime_file_unmount("/savedata0");
    remove_file(dir,"DATA.BIN");
    assert(!rmdir(dir));
}
int main(void) {
    char root[]="/tmp/minustwo-mod-files-XXXXXX";
    assert(mkdtemp(root));
    char game[512],user[512],source[512],link[512];
    snprintf(game,sizeof(game),"%s/game",root);
    snprintf(user,sizeof(user),"%s/user",root);
    snprintf(source,sizeof(source),"%s/mod.dcx",root);
    snprintf(link,sizeof(link),"%s/game/asset.dcx",root);
    assert(!mkdir(game,0755));
    FILE *f=fopen(source,"w"); assert(f); assert(fputs("modded",f)>=0); assert(!fclose(f));
    assert(!symlink(source,link));
    runtime_file_configure(game,user);
    for (int i=0;i<3;++i) {
        const char *path=i==0 ? "/app0/asset.dcx" : i==1 ? "/hostapp/asset.dcx" : "asset.dcx";
        int fd=(int)do_open(path,0,0); assert(fd>=3);
        char content[8]={0}; assert(do_read(fd,content,6)==6 && !strcmp(content,"modded"));
        GuestStat info; assert(!do_stat(path,&info) && info.size==6);
        assert(!do_close(fd));
        assert(do_open(path,2,0)==-EROFS);
        assert(do_open(path,0x400,0)==-EROFS);
        assert(do_truncate(path,0)==-EROFS);
        assert(path_op(path,2,0)==-EROFS);
        assert(do_rename(path,"/data/moved")==-EROFS);
    }
    /* An exact-file mount replaces one asset; its siblings under /app0 still come from the game. */
    char replacement[512], other[512];
    snprintf(replacement,sizeof(replacement),"%s/replacement.dcx",root);
    snprintf(other,sizeof(other),"%s/game/other.dcx",root);
    f=fopen(replacement,"w"); assert(f); assert(fputs("exact",f)>=0); assert(!fclose(f));
    f=fopen(other,"w"); assert(f); assert(fputs("plain",f)>=0); assert(!fclose(f));
    assert(!runtime_file_mount("/app0/asset.dcx",replacement));
    int exact=(int)do_open("/app0/asset.dcx",0,0); assert(exact>=3);
    char exact_content[8]={0}; assert(do_read(exact,exact_content,5)==5 && !strcmp(exact_content,"exact"));
    GuestStat exact_info; assert(!do_stat("/app0/asset.dcx",&exact_info) && exact_info.size==5);
    assert(!do_close(exact));
    assert(do_open("/app0/asset.dcx",2,0)==-EROFS);
    int sibling=(int)do_open("/app0/other.dcx",0,0); assert(sibling>=3);
    char sibling_content[8]={0}; assert(do_read(sibling,sibling_content,5)==5 && !strcmp(sibling_content,"plain"));
    assert(!do_close(sibling));
    runtime_file_unmount("/app0/asset.dcx");
    japanese_voice_files(game);
    save_language(root);
    int dir=(int)do_open("/app0",0x20000,0); assert(dir>=3);
    char entries[1024]; int64_t count=do_getdents(dir,entries,sizeof(entries),NULL); assert(count>0);
    int found=0;
    for (int64_t p=0;p<count;) {
        uint16_t length; memcpy(&length,entries+p+4,2);
        assert(length);
        if (!strcmp(entries+p+8,"asset.dcx")) { assert(entries[p+6]==8); found=1; }
        p+=length;
    }
    assert(found && !do_close(dir));
    int save=(int)do_open("/data/test-save",0x202,0644); assert(save>=3);
    assert(do_write(save,"save",4)==4 && !do_close(save));
    assert(!path_op("/data/test-save",2,0));
    assert(!unlink(link) && !unlink(other) && !unlink(source) && !unlink(replacement) && !rmdir(game));
    const char *dirs[]={"temp0","download0","data"};
    for (int i=0;i<3;++i) { char p[1024]; snprintf(p,sizeof(p),"%s/%s",user,dirs[i]); assert(!rmdir(p)); }
    assert(!rmdir(user) && !rmdir(root));
    puts("Guest mod files: reads, stat, merged listing, readonly assets, writable saves, Japanese voices and the "
         "save's language PASS");
}
