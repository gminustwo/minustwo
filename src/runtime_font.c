/* libSceFont / libSceFontFt on FreeType. GODZILLA draws its menu and subtitle text glyph by glyph
 * into its own textures with sceFontRenderCharGlyphImage, from fonts it ships (HGRPP1.TTC, FOT New
 * Cinema B) and from the console's system font set.
 *
 * The PS4 system fonts (SST) are not part of the game, so a system font set opens host fonts
 * instead: MZ_SYSTEM_FONT (one path or several separated by ';') when set, otherwise Windows' UI
 * and Japanese fonts. Every face of a font is tried in order for a missing glyph.
 *
 * Library, renderer and font handles are opaque numbers, never host pointers; one lock serializes
 * FreeType, which is not thread-safe for one library. */
#define _GNU_SOURCE
#include "runtime.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#define ERR_FATAL ((int32_t)0x80460001)
#define ERR_INVALID_PARAMETER ((int32_t)0x80460002)
#define ERR_INVALID_MEMORY ((int32_t)0x80460003)
#define ERR_INVALID_LIBRARY ((int32_t)0x80460004)
#define ERR_INVALID_FONT_HANDLE ((int32_t)0x80460005)
#define ERR_INVALID_RENDERER ((int32_t)0x80460007)
#define ERR_FS_OPEN_FAILED ((int32_t)0x80460011)
#define ERR_NO_SUPPORT_FORMAT ((int32_t)0x80460019)
#define ERR_FONT_OPEN_MAX ((int32_t)0x80460033)
#define ERR_FONT_OPEN_FAILED ((int32_t)0x80460036)
#define ERR_NO_SUPPORT_CODE ((int32_t)0x80460041)
#define ERR_NO_SUPPORT_GLYPH ((int32_t)0x80460042)
#define ERR_NO_SUPPORT_SURFACE ((int32_t)0x80460050)
#define ERR_NOT_BOUND_RENDERER ((int32_t)0x80460061)

#define HANDLE_LIBRARY 0x464c0000u
#define HANDLE_RENDERER 0x46520000u
#define HANDLE_FONT 0x46460000u
#define MAX_FONTS 256
#define MAX_FACES 4

typedef struct {
    int used;
    uint32_t library, renderer;
    FT_Face faces[MAX_FACES];
    unsigned char *owned[MAX_FACES]; /* file data FreeType reads from (fonts opened from files) */
    int face_count;
    float scale_w, scale_h, weight_x, weight_y;
    char name[96];
} Font;

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static FT_Library ft;
static Font fonts[MAX_FONTS];
static uint32_t libraries, renderers;
static size_t glyphs_rendered, glyphs_missing;
static int trace=-1;
/* MZ_FONT_TRACE=1: the scale, weight and first glyphs of every font as the game sets them. */
static int tracing(void) { if (trace<0) { const char *v=getenv("MZ_FONT_TRACE"); trace=v && *v && *v!='0'; } return trace; }

static int ft_ready(void) {
    if (!ft && FT_Init_FreeType(&ft)) { ft=NULL; fputs("Runtime: FreeType initialization failed\n",stderr); }
    return ft!=NULL;
}
static Font *font_of(uint64_t handle) {
    const uint32_t index=(uint32_t)handle-HANDLE_FONT;
    if ((handle>>32) || index>=MAX_FONTS || !fonts[index].used) return NULL;
    return &fonts[index];
}
static int library_valid(uint64_t handle) {
    return !(handle>>32) && ((uint32_t)handle & 0xffff0000u)==HANDLE_LIBRARY && ((uint32_t)handle & 0xffff)<libraries+1 && (uint32_t)handle!=HANDLE_LIBRARY;
}

/* ---- Memory, library and renderer objects ---- */
/* OrbisFontMem: kind, attributes, region size @4, region @8, mspace @16, destroy callback @24,
 * destroy argument @32. The game's region stays unused: FreeType allocates on the host heap. */
static ABI int32_t memory_init(unsigned char *mem, void *region, uint32_t size, const void *iface, void *mspace,
                               void *destroy, void *destroy_object) {
    if (!mem) return ERR_INVALID_MEMORY;
    (void)iface;
    memset(mem,0,64);
    const uint16_t kind=0x0f00;
    memcpy(mem,&kind,2);
    memcpy(mem+4,&size,4); memcpy(mem+8,&region,8); memcpy(mem+16,&mspace,8);
    memcpy(mem+24,&destroy,8); memcpy(mem+32,&destroy_object,8);
    return 0;
}
static ABI void memory_term(void *mem) { (void)mem; }
/* The FreeType module selections: any stable non-null address the game hands back. */
static const uint32_t ft_library_selection[4]={0x46540001,0,0,0}, ft_renderer_selection[4]={0x46540002,0,0,0};
static ABI const void *select_library_ft(int32_t value) { (void)value; return ft_library_selection; }
static ABI const void *select_renderer_ft(int32_t value) { (void)value; return ft_renderer_selection; }
static ABI int32_t create_library(const void *mem, const void *selection, uint64_t edition, uint64_t *out) {
    (void)edition;
    if (!mem) return ERR_INVALID_MEMORY;
    if (!selection || !out) return ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&lock);
    const int ready=ft_ready();
    if (ready) *out=HANDLE_LIBRARY+ ++libraries;
    pthread_mutex_unlock(&lock);
    if (!ready) return ERR_FATAL;
    printf("Runtime: font library %llx created (FreeType %d.%d.%d)\n",(unsigned long long)*out,FREETYPE_MAJOR,FREETYPE_MINOR,FREETYPE_PATCH);
    return 0;
}
static ABI int32_t destroy_library(uint64_t *library) {
    if (!library || !library_valid(*library)) return ERR_INVALID_LIBRARY;
    *library=0;
    return 0;
}
static ABI int32_t create_renderer(const void *mem, const void *selection, uint64_t edition, uint64_t *out) {
    (void)edition;
    if (!mem) return ERR_INVALID_MEMORY;
    if (!selection || !out) return ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&lock);
    *out=HANDLE_RENDERER+ ++renderers;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t destroy_renderer(uint64_t *renderer) {
    if (!renderer || ((uint32_t)*renderer & 0xffff0000u)!=HANDLE_RENDERER) return ERR_INVALID_RENDERER;
    *renderer=0;
    return 0;
}
static ABI int32_t library_setting(uint64_t library) { return library_valid(library) ? 0 : ERR_INVALID_LIBRARY; }
static ABI int32_t support_external(uint64_t library, uint32_t max, uint32_t formats) {
    (void)max; (void)formats;
    return library_valid(library) ? 0 : ERR_INVALID_LIBRARY;
}
static ABI int32_t attach_cache(uint64_t library, void *buffer, uint32_t size) {
    (void)buffer; (void)size;
    return library_valid(library) ? 0 : ERR_INVALID_LIBRARY;
}

/* ---- Opening fonts ---- */
/* OrbisFontOpenParams (detail): tag, pad, flags, subfont_index @8, unique_id. */
static uint32_t subfont_of(const unsigned char *detail) {
    if (!detail) return 0;
    uint32_t index; memcpy(&index,detail+8,4);
    return index<64 ? index : 0;
}
static Font *font_new(uint64_t library, uint64_t *out) {
    for (uint32_t i=0;i<MAX_FONTS;++i) if (!fonts[i].used) {
        Font *f=&fonts[i];
        memset(f,0,sizeof(*f));
        f->used=1; f->library=(uint32_t)library;
        f->scale_w=f->scale_h=16.0f; f->weight_x=f->weight_y=1.0f;
        *out=HANDLE_FONT+i;
        return f;
    }
    return NULL;
}
static void font_free(Font *f) {
    for (int i=0;i<f->face_count;++i) { FT_Done_Face(f->faces[i]); free(f->owned[i]); }
    memset(f,0,sizeof(*f));
}
static int add_face(Font *f, const unsigned char *data, uint64_t size, uint32_t subfont, unsigned char *owned) {
    FT_Face face;
    if (f->face_count==MAX_FACES || FT_New_Memory_Face(ft,data,(FT_Long)size,(FT_Long)subfont,&face)) {
        free(owned);
        return 0;
    }
    FT_Select_Charmap(face,FT_ENCODING_UNICODE);
    f->faces[f->face_count]=face; f->owned[f->face_count]=owned; ++f->face_count;
    return 1;
}
static unsigned char *read_host_file(const char *path, uint64_t *size) {
    FILE *file=fopen(path,"rb");
    if (!file) return NULL;
    unsigned char *data=NULL;
    long length=-1;
    if (!fseek(file,0,SEEK_END) && (length=ftell(file))>0 && length<256L*1024*1024 && !fseek(file,0,SEEK_SET)) {
        data=malloc((size_t)length);
        if (data && fread(data,1,(size_t)length,file)!=(size_t)length) { free(data); data=NULL; }
    }
    fclose(file);
    if (data) *size=(uint64_t)length;
    return data;
}
static unsigned char *read_guest_file(const char *path, uint64_t *size) {
    int64_t fd=runtime_file_open(path,0,0);
    if (fd<0) return NULL;
    unsigned char *data=NULL;
    const int64_t length=runtime_file_lseek((int)fd,0,2);
    if (length>0 && length<256LL*1024*1024 && runtime_file_lseek((int)fd,0,0)==0) {
        data=malloc((size_t)length);
        if (data && runtime_file_read((int)fd,data,(uint64_t)length)!=length) { free(data); data=NULL; }
    }
    runtime_file_close((int)fd);
    if (data) *size=(uint64_t)length;
    return data;
}
static ABI int32_t open_memory(uint64_t library, const unsigned char *address, uint32_t size, const unsigned char *detail, uint64_t *out) {
    if (!library_valid(library)) return ERR_INVALID_LIBRARY;
    if (!address || !size || !out) return ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&lock);
    Font *f=font_new(library,out);
    int32_t result=!f ? ERR_FONT_OPEN_MAX : add_face(f,address,size,subfont_of(detail),NULL) ? 0 : ERR_NO_SUPPORT_FORMAT;
    if (f && result) font_free(f);
    if (!result) snprintf(f->name,sizeof(f->name),"%s %s",f->faces[0]->family_name ? f->faces[0]->family_name : "?",
                          f->faces[0]->style_name ? f->faces[0]->style_name : "");
    pthread_mutex_unlock(&lock);
    if (!result) printf("Runtime: font %llx opened from memory: %s (%u bytes)\n",(unsigned long long)*out,f->name,size);
    return result;
}
static ABI int32_t open_file(uint64_t library, const char *path, uint32_t mode, const unsigned char *detail, uint64_t *out) {
    (void)mode;
    if (!library_valid(library)) return ERR_INVALID_LIBRARY;
    if (!path || !out) return ERR_INVALID_PARAMETER;
    uint64_t size=0;
    unsigned char *data=read_guest_file(path,&size);
    if (!data) { printf("Runtime: font file %s not found\n",path); return ERR_FS_OPEN_FAILED; }
    pthread_mutex_lock(&lock);
    Font *f=font_new(library,out);
    int32_t result=!f ? ERR_FONT_OPEN_MAX : add_face(f,data,size,subfont_of(detail),data) ? 0 : ERR_NO_SUPPORT_FORMAT;
    if (!f) free(data);
    if (f && result) font_free(f);
    if (!result) snprintf(f->name,sizeof(f->name),"%s",path);
    pthread_mutex_unlock(&lock);
    if (!result) printf("Runtime: font %llx opened from %s\n",(unsigned long long)*out,path);
    return result;
}
/* A system font set: MZ_SYSTEM_FONT, else Windows' UI font for Latin text followed by its
 * Japanese fonts (Japanese menus, katakana names). */
static ABI int32_t open_set(uint64_t library, uint32_t type, uint32_t mode, const unsigned char *detail, uint64_t *out) {
    (void)mode; (void)detail;
    if (!library_valid(library)) return ERR_INVALID_LIBRARY;
    if (!out) return ERR_INVALID_PARAMETER;
    char paths[2048];
    const char *chosen=getenv("MZ_SYSTEM_FONT");
#ifdef _WIN32
    const char *windir=getenv("WINDIR");
    if (!windir || !*windir) windir="C:\\Windows";
    if (!chosen || !*chosen) {
        snprintf(paths,sizeof(paths),"%s\\Fonts\\segoeui.ttf;%s\\Fonts\\YuGothM.ttc;%s\\Fonts\\meiryo.ttc;%s\\Fonts\\msgothic.ttc;%s\\Fonts\\arial.ttf",
                 windir,windir,windir,windir,windir);
        chosen=paths;
    } else snprintf(paths,sizeof(paths),"%s",chosen);
#else
    if (!chosen || !*chosen)
        chosen="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf;/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc;"
               "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc;/usr/share/fonts/TTF/DejaVuSans.ttf";
    snprintf(paths,sizeof(paths),"%s",chosen);
#endif
    pthread_mutex_lock(&lock);
    Font *f=font_new(library,out);
    if (!f) { pthread_mutex_unlock(&lock); return ERR_FONT_OPEN_MAX; }
    for (char *path=paths, *next; path && f->face_count<MAX_FACES; path=next) {
        next=strchr(path,';');
        if (next) *next++=0;
        uint64_t size=0;
        unsigned char *data=*path ? read_host_file(path,&size) : NULL;
        if (data) add_face(f,data,size,0,data);
    }
    const int faces=f->face_count;
    if (!faces) font_free(f);
    else snprintf(f->name,sizeof(f->name),"system set 0x%08x (%s)",type,f->faces[0]->family_name ? f->faces[0]->family_name : "?");
    pthread_mutex_unlock(&lock);
    if (!faces) { printf("Runtime: font set 0x%08x: no host font found (set MZ_SYSTEM_FONT)\n",type); return ERR_FONT_OPEN_FAILED; }
    printf("Runtime: font %llx is %s, %d faces\n",(unsigned long long)*out,f->name,faces);
    return 0;
}
static ABI int32_t close_font(uint64_t handle) {
    pthread_mutex_lock(&lock);
    Font *f=font_of(handle);
    if (f) font_free(f);
    pthread_mutex_unlock(&lock);
    return f ? 0 : ERR_INVALID_FONT_HANDLE;
}
static ABI int32_t bind_renderer(uint64_t handle, uint64_t renderer) {
    if (((uint32_t)renderer & 0xffff0000u)!=HANDLE_RENDERER) return ERR_INVALID_RENDERER;
    pthread_mutex_lock(&lock);
    Font *f=font_of(handle);
    if (f) f->renderer=(uint32_t)renderer;
    pthread_mutex_unlock(&lock);
    return f ? 0 : ERR_INVALID_FONT_HANDLE;
}
static ABI int32_t scale_pixel(uint64_t handle, float w, float h) {
    if (tracing()) printf("Font trace: %llx scale request %f x %f px\n",(unsigned long long)handle,w,h);
    if (!(w>0.0f) || !(h>0.0f) || w>4096.0f || h>4096.0f) return ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&lock);
    Font *f=font_of(handle);
    if (f) { f->scale_w=w; f->scale_h=h; }
    pthread_mutex_unlock(&lock);
    if (tracing()) printf("Font trace: %llx scale %.2f x %.2f px\n",(unsigned long long)handle,w,h);
    return f ? 0 : ERR_INVALID_FONT_HANDLE;
}
static ABI int32_t effect_weight(uint64_t handle, float x, float y, uint32_t mode) {
    (void)mode;
    pthread_mutex_lock(&lock);
    Font *f=font_of(handle);
    if (f) { f->weight_x=x>0.0f && x<4.0f ? x : 1.0f; f->weight_y=y>0.0f && y<4.0f ? y : 1.0f; }
    pthread_mutex_unlock(&lock);
    if (tracing()) printf("Font trace: %llx weight %.3f x %.3f mode %u\n",(unsigned long long)handle,x,y,mode);
    return f ? 0 : ERR_INVALID_FONT_HANDLE;
}
static void size_face(const Font *f, FT_Face face) {
    FT_Set_Char_Size(face,(FT_F26Dot6)lrintf(f->scale_w*64.0f),(FT_F26Dot6)lrintf(f->scale_h*64.0f),72,72);
}
/* OrbisFontHorizontalLayout: baseline offset (ascent), line advance, decoration extent; pixels. */
static ABI int32_t horizontal_layout(uint64_t handle, float *layout) {
    if (!layout) return ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&lock);
    Font *f=font_of(handle);
    if (!f) { pthread_mutex_unlock(&lock); memset(layout,0,12); return ERR_INVALID_FONT_HANDLE; }
    FT_Face face=f->faces[0];
    size_face(f,face);
    layout[0]=(float)face->size->metrics.ascender/64.0f;
    layout[1]=(float)face->size->metrics.height/64.0f;
    layout[2]=f->weight_y>1.0f ? (f->weight_y-1.0f)*f->scale_h : 0.0f;
    pthread_mutex_unlock(&lock);
    return 0;
}

/* ---- Rendering ---- */
/* OrbisFontRenderSurface: buffer, widthByte @8, pixelSizeByte @12, styleFlag @14, width @16,
 * height @20, scissor x0 @24, y0 @28, x1 @32, y1 @36 (128 bytes). */
typedef struct {
    unsigned char *buffer;
    int32_t width_byte;
    int8_t pixel_size;
    uint8_t pad0, style, pad2;
    int32_t width, height;
    uint32_t sc_x0, sc_y0, sc_x1, sc_y1;
    uint64_t reserved[11];
} Surface;
_Static_assert(sizeof(Surface)==128,"OrbisFontRenderSurface layout");
static ABI void surface_init(Surface *s, void *buffer, int32_t width_byte, int32_t pixel_size, int32_t width, int32_t height) {
    if (!s) return;
    memset(s,0,sizeof(*s));
    s->buffer=buffer; s->width_byte=width_byte; s->pixel_size=(int8_t)pixel_size;
    s->width=width<0 ? 0 : width; s->height=height<0 ? 0 : height;
    s->sc_x1=(uint32_t)s->width; s->sc_y1=(uint32_t)s->height;
}
/* OrbisFontGlyphMetrics: width, height, horizontal bearing x/y and advance, vertical ones (32 bytes).
 * OrbisFontRenderOutput: stage @0, surface image (address @8, widthByte @16, pixelSize @20,
 * format @21), update rectangle @24, image metrics @40 (bearing x/y, advance, stride, width,
 * height); 64 bytes. */
typedef struct { float width, height, h_bearing_x, h_bearing_y, h_advance, v_bearing_x, v_bearing_y, v_advance; } Metrics;
typedef struct {
    const void *stage;
    unsigned char *address; uint32_t width_byte; uint8_t pixel_size, format; uint16_t pad;
    uint32_t x, y, w, h;
    float bearing_x, bearing_y, advance, stride;
    uint32_t image_width, image_height;
} Output;
_Static_assert(sizeof(Metrics)==32 && sizeof(Output)==64,"sceFont render output layouts");
static int floor_int(float v) { return (int)floorf(v); }
static int ceil_int(float v) { return (int)ceilf(v); }

static int32_t render_char_core(uint64_t handle, uint32_t code, Surface *s, float x, float y, Metrics *metrics, Output *out) {
    if (metrics) memset(metrics,0,sizeof(*metrics));
    if (out) memset(out,0,sizeof(*out));
    if (!code) return ERR_NO_SUPPORT_CODE;
    if (!s || !metrics || !out) return ERR_INVALID_PARAMETER;
    if (!s->buffer || s->width<=0 || s->height<=0 || s->width_byte<=0 || (s->pixel_size!=1 && s->pixel_size!=4))
        return ERR_NO_SUPPORT_SURFACE;
    pthread_mutex_lock(&lock);
    Font *f=font_of(handle);
    if (!f) { pthread_mutex_unlock(&lock); return ERR_INVALID_FONT_HANDLE; }
    if (!f->renderer) { pthread_mutex_unlock(&lock); return ERR_NOT_BOUND_RENDERER; }
    FT_Face face=NULL;
    FT_UInt glyph=0;
    for (int i=0;i<f->face_count && !glyph;++i) {
        glyph=FT_Get_Char_Index(f->faces[i],code);
        if (glyph) face=f->faces[i];
    }
    if (!glyph) { ++glyphs_missing; pthread_mutex_unlock(&lock); return ERR_NO_SUPPORT_GLYPH; }
    /* y is the top of the line: the glyph sits on the baseline below it, at the font's baseline
     * offset (sceFontGetHorizontalLayout), as the console's library places it. */
    size_face(f,f->faces[0]);
    y+=(float)f->faces[0]->size->metrics.ascender/64.0f;
    size_face(f,face);
    FT_Vector delta={(FT_Pos)lrintf((x-floorf(x))*64.0f),(FT_Pos)-lrintf((y-floorf(y))*64.0f)};
    FT_Set_Transform(face,NULL,&delta);
    int failed=FT_Load_Glyph(face,glyph,FT_LOAD_NO_HINTING|FT_LOAD_NO_BITMAP)!=0;
    FT_Set_Transform(face,NULL,NULL);
    FT_GlyphSlot slot=face->glyph;
    if (!failed && slot->format==FT_GLYPH_FORMAT_OUTLINE && (f->weight_x>1.0f || f->weight_y>1.0f))
        FT_Outline_EmboldenXY(&slot->outline,(FT_Pos)lrintf((f->weight_x-1.0f)*f->scale_w*64.0f),
                              (FT_Pos)lrintf((f->weight_y-1.0f)*f->scale_h*64.0f));
    if (failed || FT_Render_Glyph(slot,FT_RENDER_MODE_NORMAL)) { ++glyphs_missing; pthread_mutex_unlock(&lock); return ERR_NO_SUPPORT_GLYPH; }
    metrics->width=(float)slot->metrics.width/64.0f;
    metrics->height=(float)slot->metrics.height/64.0f;
    metrics->h_bearing_x=(float)slot->metrics.horiBearingX/64.0f;
    metrics->h_bearing_y=(float)slot->metrics.horiBearingY/64.0f;
    metrics->h_advance=(float)slot->metrics.horiAdvance/64.0f;
    metrics->v_bearing_x=(float)slot->metrics.vertBearingX/64.0f;
    metrics->v_bearing_y=(float)slot->metrics.vertBearingY/64.0f;
    metrics->v_advance=(float)slot->metrics.vertAdvance/64.0f;
    /* Coverage goes where the pen is: x on the left side bearing, y on the baseline. */
    const FT_Bitmap *bitmap=&slot->bitmap;
    const int glyph_w=(int)bitmap->width, glyph_h=(int)bitmap->rows;
    const int dest_x=floor_int(x)+slot->bitmap_left, dest_y=floor_int(y)-slot->bitmap_top;
    const int clip_x0=(int)(s->sc_x0<(uint32_t)s->width ? s->sc_x0 : (uint32_t)s->width);
    const int clip_y0=(int)(s->sc_y0<(uint32_t)s->height ? s->sc_y0 : (uint32_t)s->height);
    const int clip_x1=(int)(s->sc_x1<(uint32_t)s->width ? s->sc_x1 : (uint32_t)s->width);
    const int clip_y1=(int)(s->sc_y1<(uint32_t)s->height ? s->sc_y1 : (uint32_t)s->height);
    const int x0=dest_x>clip_x0 ? dest_x : clip_x0, y0=dest_y>clip_y0 ? dest_y : clip_y0;
    const int x1=dest_x+glyph_w<clip_x1 ? dest_x+glyph_w : clip_x1, y1=dest_y+glyph_h<clip_y1 ? dest_y+glyph_h : clip_y1;
    const int bpp=s->pixel_size;
    for (int row=y0;row<y1;++row) {
        const unsigned char *src=bitmap->buffer+(row-dest_y)*bitmap->pitch;
        unsigned char *dst=s->buffer+(size_t)row*(size_t)s->width_byte;
        for (int col=x0;col<x1;++col) {
            const int sx=col-dest_x;
            const unsigned char coverage=bitmap->pixel_mode==FT_PIXEL_MODE_MONO ? ((src[sx>>3]>>(7-(sx&7)))&1)*255 : src[sx];
            unsigned char *px=dst+(size_t)col*(size_t)bpp;
            if (bpp==1) px[0]=coverage;
            else px[0]=px[1]=px[2]=px[3]=coverage;
        }
    }
    const int update_x=x1>x0 ? x0 : (dest_x>0 ? dest_x : 0), update_y=y1>y0 ? y0 : (dest_y>0 ? dest_y : 0);
    out->address=s->buffer+(size_t)update_y*(size_t)s->width_byte+(size_t)update_x*(size_t)bpp;
    out->width_byte=(uint32_t)s->width_byte; out->pixel_size=(uint8_t)bpp;
    out->x=(uint32_t)update_x; out->y=(uint32_t)update_y;
    out->w=(uint32_t)(x1>x0 ? x1-x0 : 0); out->h=(uint32_t)(y1>y0 ? y1-y0 : 0);
    const float left=x+metrics->h_bearing_x, top=y+metrics->h_bearing_y;
    const float right=left+metrics->width, bottom=top-metrics->height;
    const float advance=x+metrics->h_advance;
    out->bearing_x=(float)floor_int(left)-x;
    out->bearing_y=(float)ceil_int(top)-y;
    out->advance=(float)ceil_int(advance)-x;
    out->stride=(float)(ceil_int(right)>ceil_int(advance) ? ceil_int(right) : ceil_int(advance))-x;
    out->image_width=(uint32_t)(ceil_int(right)-floor_int(left)>0 ? ceil_int(right)-floor_int(left) : 0);
    out->image_height=(uint32_t)(ceil_int(top)-floor_int(bottom)>0 ? ceil_int(top)-floor_int(bottom) : 0);
    ++glyphs_rendered;
    if (tracing() && glyphs_rendered<400)
        printf("Font trace: %llx U+%04X at %.1f,%.1f on %dx%d (%d B/px, scissor %u,%u-%u,%u) -> rect %u,%u %ux%u, image %ux%u bearing %.1f,%.1f advance %.1f\n",
               (unsigned long long)handle,code,x,y,s->width,s->height,bpp,s->sc_x0,s->sc_y0,s->sc_x1,s->sc_y1,out->x,out->y,out->w,
               out->h,out->image_width,out->image_height,out->bearing_x,out->bearing_y,out->advance);
    pthread_mutex_unlock(&lock);
    return 0;
}

static ABI int32_t render_char(uint64_t handle, uint32_t code, Surface *s, float x, float y, Metrics *metrics, Output *out) {
    const int32_t result=render_char_core(handle,code,s,x,y,metrics,out);
    static unsigned failures;
    if (result && tracing() && failures++<100)
        printf("Font trace: %llx U+%04X at %.1f,%.1f failed 0x%08x (surface %p %dx%d, %d B/px, row %d)\n",
               (unsigned long long)handle,code,x,y,(unsigned)result,s ? (void *)s->buffer : NULL,s ? s->width : 0,
               s ? s->height : 0,s ? s->pixel_size : 0,s ? s->width_byte : 0);
    return result;
}

static const RuntimeExport exports[]={
    {"sceFontMemoryInit",memory_init}, {"sceFontMemoryTerm",memory_term},
    {"sceFontSelectLibraryFt",select_library_ft}, {"sceFontSelectRendererFt",select_renderer_ft},
    {"sceFontCreateLibraryWithEdition",create_library}, {"sceFontDestroyLibrary",destroy_library},
    {"sceFontCreateRendererWithEdition",create_renderer}, {"sceFontDestroyRenderer",destroy_renderer},
    {"sceFontSupportSystemFonts",library_setting}, {"sceFontSupportExternalFonts",support_external},
    {"sceFontAttachDeviceCacheBuffer",attach_cache},
    {"sceFontOpenFontMemory",open_memory}, {"sceFontOpenFontFile",open_file}, {"sceFontOpenFontSet",open_set},
    {"sceFontCloseFont",close_font}, {"sceFontBindRenderer",bind_renderer},
    {"sceFontSetupRenderScalePixel",scale_pixel}, {"sceFontSetupRenderEffectWeight",effect_weight},
    {"sceFontGetHorizontalLayout",horizontal_layout},
    {"sceFontRenderSurfaceInit",surface_init}, {"sceFontRenderCharGlyphImage",render_char},
};
uintptr_t runtime_font_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_font_report(void) {
    printf("Runtime: fonts glyphs rendered=%zu, missing=%zu\n",glyphs_rendered,glyphs_missing);
}
