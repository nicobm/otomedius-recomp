/* d3d9_objects.c - textures, buffers, surfaces and shaders. */

#define _GNU_SOURCE
#include "win32.h"
#include "d3d9.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    uint32_t vt, refcount;
    uint32_t type;
    uint32_t w, h, levels, usage, format, pool;
    uint32_t data;          /* emulated pointer to pixel data */
    uint32_t data_size;
    int      fmt;           /* our image format id */
} texture_t;

typedef struct {
    uint32_t vt, refcount;
    uint32_t size, usage, fvf, format;
    uint32_t data;
    int is_index;
} buffer_t;

typedef struct {
    uint32_t vt, refcount;
    uint32_t w, h, format, renderable;
    uint32_t data;
    int      is_backbuffer;     /* data aliases the renderer's framebuffer */
} surface_t_;

typedef struct {
    uint32_t vt, refcount;
    uint32_t code;
    int pixel;
} shader_t;

static texture_t *tex(uint32_t o) { return (texture_t *)(MEM + o); }
static buffer_t *buf(uint32_t o) { return (buffer_t *)(MEM + o); }
static surface_t_ *surf(uint32_t o) { return (surface_t_ *)(MEM + o); }
static shader_t *shad(uint32_t o) { return (shader_t *)(MEM + o); }

/* ------------------------------------------------------------- textures */
static void t_QueryInterface(void) { RET32(0); }
static void t_AddRef(void) { M32(arg32(0) + 4)++; RET32(M32(arg32(0) + 4)); }
static void t_Release(void) { RET32(0); }
static void t_GetDevice(void) { RET32(0); }
static void t_SetPrivateData(void) { RET32(0); }
static void t_GetPrivateData(void) { RET32(0); }
static void t_FreePrivateData(void) { RET32(0); }
static void t_SetPriority(void) { RET32(0); }
static void t_GetPriority(void) { RET32(0); }
static void t_PreLoad(void) {}
static void t_GetType(void) { RET32(1); }
static void t_SetLOD(void) { RET32(0); }
static void t_GetLOD(void) { RET32(0); }
static void t_GetLevelCount(void) { RET32(tex(arg32(0))->levels); }
static void t_SetAutoGenFilterType(void) { RET32(0); }
static void t_GetAutoGenFilterType(void) { RET32(0); }
static void t_GenerateMipSubLevels(void) {}

static void t_GetLevel(void)
{
    uint32_t pp = arg32(2);
    if (pp)
        M32(pp) = surface_create(tex(arg32(0))->w, tex(arg32(0))->h,
                                 tex(arg32(0))->format, 0);
    RET32(0);
}

static void t_GetLevelDesc(void)
{
    /* IDirect3DTexture9::GetLevelDesc(Level, D3DSURFACE_DESC *desc).
     * The movie renderer asks for the level description and only accepts a
     * texture whose format is X8R8G8B8 (0x16) or A8R8G8B8 (0x19); a stub that
     * left the structure zeroed made every video media type be rejected with
     * VFW_E_TYPE_NOT_ACCEPTED. */
    uint32_t self = arg32(0), desc = arg32(2);
    texture_t *t = tex(self);
    if (!t || !desc) {
        RET32(0x8876086Au);
        return;
    }
    M32(desc + 0) = t->format;      /* Format */
    M32(desc + 4) = 1;              /* Type = D3DRTYPE_TEXTURE */
    M32(desc + 8) = t->usage;       /* Usage */
    M32(desc + 12) = t->pool;       /* Pool */
    M32(desc + 16) = 0;             /* MultiSampleType */
    M32(desc + 20) = 0;             /* MultiSampleQuality */
    M32(desc + 24) = t->w;          /* Width */
    M32(desc + 28) = t->h;          /* Height */
    if (getenv("OTOMEDIUS_MOVIE_DBG"))
        fprintf(stderr, "d3d9: GetLevelDesc(%08x) fmt=%x %ux%u -> %08x\n",
                self, t->format, t->w, t->h, desc);
    RET32(0);
}
/* IDirect3DTexture9::LockRect(Level, D3DLOCKED_RECT *locked, RECT *rect,
 * DWORD flags) - the game uploads every texture by locking it and copying its
 * decoded pixels in, so returning D3DERR_INVALIDCALL here left every texture
 * blank (all-black preview, invisible sprites). */
static void t_LockRect(void)
{
    uint32_t self = arg32(0);
    uint32_t locked = arg32(2);
    texture_t *t = tex(self);

    /* Real D3D9 succeeds here and the game builds its font atlases through
     * this path (CTexFont -> GetGlyphOutline -> LockRect).  That is now the
     * default; OTOMEDIUS_TEXTURES=0 restores the old "always fail" behaviour
     * for bisecting the loader/render paths. */
    {
        const char *e = getenv("OTOMEDIUS_TEXTURES");
        if (e && *e && *e == '0') {
            RET32(0x8876086Au);
            return;
        }
    }
    if (!t || !t->data) {
        RET32(0x8876086Au);   /* D3DERR_INVALIDCALL */
        return;
    }
    if (locked) {
        M32(locked + 0) = t->w * 4u;   /* Pitch */
        M32(locked + 4) = t->data;     /* pBits, emulated pointer */
    }
    {
        static int n;
        if (g_verbose && n++ < 16)
            fprintf(stderr, "d3d9: tex LockRect(%08x) %ux%u -> %08x\n",
                    self, t->w, t->h, t->data);
    }
    RET32(0);
}
static void t_UnlockRect(void)
{
    if (getenv("OTOMEDIUS_TEXDBG")) {
        texture_t *t = tex(arg32(0));
        if (t && t->data && t->w && t->h) {
            const uint32_t *p = (const uint32_t *)(MEM + t->data);
            uint32_t nz = 0, a0 = 0, first = 0, first_i = 0;
            for (uint32_t i = 0, n = t->w * t->h; i < n; i++) {
                uint32_t c = p[i];
                if (c & 0x00FFFFFFu) {
                    if (!nz) { first = c; first_i = i; }
                    nz++;
                    if (!(c & 0xFF000000u)) a0++;
                }
            }
            fprintf(stderr, "d3d9: unlock tex %08x %ux%u: rgb=%u alpha0=%u "
                    "first=%08x @%u\n", arg32(0), t->w, t->h, nz, a0, first,
                    first_i);
            if (t->w <= 2048 && t->h <= 2048) {
                char path[160];
                FILE *fp;
                snprintf(path, sizeof(path), "/tmp/opencode/tex_%08x.pgm",
                         arg32(0));
                fp = fopen(path, "wb");
                if (fp) {
                    fprintf(fp, "P5\n%u %u\n255\n", t->w, t->h);
                    for (uint32_t i = 0, n = t->w * t->h; i < n; i++)
                        fputc((int)(p[i] >> 24), fp);
                    fclose(fp);
                    fprintf(stderr, "d3d9:   wrote %s\n", path);
                }
            }
        }
    }
    RET32(0);
}
static void t_AddDirtyRect(void) { RET32(0); }

/* The engine uploads pixels through the level-0 surface
 * (GetSurfaceLevel -> LockRect -> memcpy), so the surface must share the
 * texture's pixel buffer - the old code gave it a private one and every
 * upload went to memory nobody rendered from. */
static void t_GetSurfaceLevel(void)
{
    uint32_t level = arg32(1), pp = arg32(2);
    texture_t *t = tex(arg32(0));
    uint32_t s;
    if (!pp)
        return;
    if (!t) {
        M32(pp) = 0;
        RET32(0x8876086Au);
        return;
    }
    s = surface_create(t->w, t->h, t->format, 1);
    if (s) {
        surf(s)->data = t->data;
        surf(s)->w = t->w;
        surf(s)->h = t->h;
    }
    (void)level;
    M32(pp) = s;
    RET32(0);
}

static const method_t texture_methods[] = {
    {t_QueryInterface, 3},   /* QueryInterface */
    {t_AddRef, 1},   /* AddRef */
    {t_Release, 1},   /* Release */
    {t_GetDevice, 2},   /* GetDevice */
    {t_SetPrivateData, 5},   /* SetPrivateData */
    {t_GetPrivateData, 4},   /* GetPrivateData */
    {t_FreePrivateData, 2},   /* FreePrivateData */
    {t_SetPriority, 2},   /* SetPriority */
    {t_GetPriority, 1},   /* GetPriority */
    {t_PreLoad, 1},   /* PreLoad */
    {t_GetType, 1},   /* GetType */
    {t_SetLOD, 2},   /* SetLOD */
    {t_GetLOD, 1},   /* GetLOD */
    {t_GetLevelCount, 1},   /* GetLevelCount */
    {t_SetAutoGenFilterType, 2},   /* SetAutoGenFilterType */
    {t_GetAutoGenFilterType, 1},   /* GetAutoGenFilterType */
    {t_GenerateMipSubLevels, 1},   /* GenerateMipSubLevels */
    {t_GetLevelDesc, 3},   /* GetLevelDesc */
    {t_GetSurfaceLevel, 3},   /* GetSurfaceLevel */
    {t_LockRect, 5},   /* LockRect */
    {t_UnlockRect, 2},   /* UnlockRect */
    {t_AddDirtyRect, 2},   /* AddDirtyRect */
};

uint32_t texture_create(uint32_t w, uint32_t h, uint32_t levels, uint32_t usage,
                        uint32_t format, uint32_t pool)
{
    uint32_t obj = com_create_named("IDirect3DTexture9", texture_methods,
                              (int)(sizeof(texture_methods) / sizeof(method_t)),
                              sizeof(texture_t));
    texture_t *t = tex(obj);
    t->type = 1;
    t->w = w;
    t->h = h;
    t->levels = levels ? levels : 1;
    t->usage = usage;
    t->format = format;
    t->pool = pool;
    t->data = emu_alloc(w * h * 4 + 64);
    t->data_size = w * h * 4;
    if (getenv("OTOMEDIUS_TEXDBG"))
        fprintf(stderr, "texdbg: CreateTexture %08x %ux%u fmt=%u levels=%u "
                "usage=%x pool=%u\n", obj, w, h, format, levels, usage, pool);
    if (g_verbose > 1)
        fprintf(stderr, "d3d9: CreateTexture(%ux%u fmt %u) -> %08x data %08x\n",
                w, h, format, obj, t->data);
    return obj;
}

void texture_upload(uint32_t obj, const void *data, uint32_t size, int fmt)
{
    texture_t *t = tex(obj);
    if (!t)
        return;
    if (size > t->data_size)
        size = t->data_size;
    memcpy(MEM + t->data, data, size);
    t->fmt = fmt;
    if (getenv("OTOMEDIUS_TEXDBG"))
        fprintf(stderr, "texdbg: upload %08x fmt=%d %u bytes\n", obj, fmt, size);
    if (g_verbose > 1)
        fprintf(stderr, "d3d9: texture %08x %ux%u uploaded (%u bytes, fmt %d)\n",
                obj, t->w, t->h, size, fmt);
}

/* -------------------------------------------------------------- buffers */
static void b_QueryInterface(void) { RET32(0); }
static void b_AddRef(void) { M32(arg32(0) + 4)++; RET32(M32(arg32(0) + 4)); }
static void b_Release(void) { RET32(0); }
static void b_GetDevice(void) { RET32(0); }
static void b_SetPrivateData(void) { RET32(0); }
static void b_GetPrivateData(void) { RET32(0); }
static void b_FreePrivateData(void) { RET32(0); }
static void b_SetPriority(void) { RET32(0); }
static void b_GetPriority(void) { RET32(0); }
static void b_PreLoad(void) {}
static void b_GetType(void) { RET32(0); }

static void b_Lock(void)
{
    uint32_t o = arg32(0), offset = arg32(1), flags = arg32(2), pp = arg32(3);
    buffer_t *b = buf(o);
    uint32_t ptr = b->data + offset;
    if (flags & 0x2000)          /* D3DLOCK_DISCARD */
        ptr = b->data;
    if (pp)
        M32(pp) = ptr;
    RET32(0);
}

static void b_Unlock(void) { RET32(0); }

static void b_GetDesc(void)
{
    uint32_t o = arg32(0), p = arg32(1);
    buffer_t *b = buf(o);
    if (p) {
        M32(p + 0) = b->format;
        M32(p + 4) = b->is_index ? 0 : 0;   /* Type */
        M32(p + 8) = b->usage;
        M32(p + 12) = b->is_index ? 0 : b->fvf;
        M32(p + 16) = b->is_index ? 0 : 1;  /* Pool */
        M32(p + 20) = b->size;
    }
    RET32(0);
}

static const method_t buffer_methods[] = {
    {b_QueryInterface, 3},   /* QueryInterface */
    {b_AddRef, 1},   /* AddRef */
    {b_Release, 1},   /* Release */
    {b_GetDevice, 2},   /* GetDevice */
    {b_SetPrivateData, 5},   /* SetPrivateData */
    {b_GetPrivateData, 4},   /* GetPrivateData */
    {b_FreePrivateData, 2},   /* FreePrivateData */
    {b_SetPriority, 2},   /* SetPriority */
    {b_GetPriority, 1},   /* GetPriority */
    {b_PreLoad, 1},   /* PreLoad */
    {b_GetType, 1},   /* GetType */
    {b_Lock, 5},   /* Lock */
    {b_Unlock, 1},   /* Unlock */
    {b_GetDesc, 2},   /* GetDesc */
};

const void *buffer_data(uint32_t obj, uint32_t *size, int *is_index,
                        uint32_t *format)
{
    buffer_t *b;
    if (!obj)
        return NULL;
    b = buf(obj);
    if (b->size > 0x40000000u)
        return NULL;
    if (size)
        *size = b->size;
    if (is_index)
        *is_index = b->is_index;
    if (format)
        *format = b->format;
    return MEM + b->data;
}

uint32_t buffer_create(uint32_t size, uint32_t usage, uint32_t fvf, int is_index)
{
    uint32_t obj = com_create_named("IDirect3DVertexBuffer9", buffer_methods,
                              (int)(sizeof(buffer_methods) / sizeof(method_t)),
                              sizeof(buffer_t));
    buffer_t *b = buf(obj);
    b->size = size;
    b->usage = usage;
    b->fvf = fvf;
    b->is_index = is_index;
    b->data = emu_alloc(size + 16);
    return obj;
}

/* ------------------------------------------------------------- surfaces */
static void s_QueryInterface(void) { RET32(0); }
static void s_AddRef(void) { M32(arg32(0) + 4)++; RET32(M32(arg32(0) + 4)); }
static void s_Release(void) { RET32(0); }
static void s_GetDevice(void) { RET32(0); }
static void s_SetPrivateData(void) { RET32(0); }
static void s_GetPrivateData(void) { RET32(0); }
static void s_FreePrivateData(void) { RET32(0); }
static void s_SetPriority(void) { RET32(0); }
static void s_GetPriority(void) { RET32(0); }
static void s_PreLoad(void) {}
static void s_GetType(void) { RET32(1); }
static void s_GetDesc(void)
{
    /* D3DSURFACE_DESC: Format, Type, Usage, Pool, MultiSampleType,
     * MultiSampleQuality, Width, Height.  The scene code uses the back
     * buffer's format/size to create its render target texture. */
    surface_t_ *s = surf(arg32(0));
    uint32_t d = arg32(1);
    if (!s || !d) {
        RET32(0x8876086Au);
        return;
    }
    M32(d + 0)  = s->format;
    M32(d + 4)  = 2;                 /* D3DRTYPE_SURFACE */
    M32(d + 8)  = 0;                 /* Usage */
    M32(d + 12) = 0;                 /* Pool */
    M32(d + 16) = 0;                 /* MultiSampleType */
    M32(d + 20) = 0;                 /* MultiSampleQuality */
    M32(d + 24) = s->w;
    M32(d + 28) = s->h;
    RET32(0);
}
/* IDirect3DSurface9::LockRect(D3DLOCKED_RECT *locked, RECT *rect, DWORD flags) */
static void s_LockRect(void)
{
    surface_t_ *s = surf(arg32(0));
    uint32_t locked = arg32(1);
    if (!s || !s->data) {
        RET32(0x8876086Au);
        return;
    }
    if (locked) {
        M32(locked + 0) = s->w * 4u;
        M32(locked + 4) = s->data;
    }
    RET32(0);
}
static void s_UnlockRect(void) { RET32(0); }
static void s_GetDC(void) { RET32(0xDC000002u); }
static void s_ReleaseDC(void) { RET32(0); }

static void s_GetContainer(void) { RET32(0); }

static const method_t surface_methods[] = {
    {s_QueryInterface, 3},   /* QueryInterface */
    {s_AddRef, 1},   /* AddRef */
    {s_Release, 1},   /* Release */
    {s_GetDevice, 2},   /* GetDevice */
    {s_SetPrivateData, 5},   /* SetPrivateData */
    {s_GetPrivateData, 4},   /* GetPrivateData */
    {s_FreePrivateData, 2},   /* FreePrivateData */
    {s_SetPriority, 2},   /* SetPriority */
    {s_GetPriority, 1},   /* GetPriority */
    {s_PreLoad, 1},   /* PreLoad */
    {s_GetType, 1},   /* GetType */
    {s_GetContainer, 3},   /* GetContainer */
    {s_GetDesc, 2},   /* GetDesc */
    {s_LockRect, 4},   /* LockRect */
    {s_UnlockRect, 1},   /* UnlockRect */
    {s_GetDC, 2},   /* GetDC */
    {s_ReleaseDC, 2},   /* ReleaseDC */
};

/* The D3D back buffer is not a separate allocation in this port: point the
 * surface at the renderer's framebuffer so LockRect / GetRenderTargetData see
 * what the rasteriser drew. */
void surface_alias_backbuffer(uint32_t s)
{
    int w = 0, h = 0;
    uint32_t fb = render_framebuffer_guest();
    if (!s || !surf(s) || !fb)
        return;
    (void)render_framebuffer(&w, &h);
    surf(s)->data = fb;
    surf(s)->w = (uint32_t)(w > 0 ? w : surf(s)->w);
    surf(s)->h = (uint32_t)(h > 0 ? h : surf(s)->h);
    surf(s)->is_backbuffer = 1;
}

uint32_t surface_pixels(uint32_t s) { return surf(s) ? surf(s)->data : 0; }
int surface_width(uint32_t s) { return surf(s) ? (int)surf(s)->w : 0; }
int surface_height(uint32_t s) { return surf(s) ? (int)surf(s)->h : 0; }

/* GetRenderTargetData / StretchRect: copy the source surface into the
 * destination pixel buffer (both are ARGB, top-down, pitch = width * 4). */
int surface_copy(uint32_t dst, uint32_t src)
{
    surface_t_ *d = surf(dst), *s = surf(src);
    int w, h, y;
    if (!d || !s || !d->data || !s->data)
        return 0;
    w = (int)(d->w < s->w ? d->w : s->w);
    h = (int)(d->h < s->h ? d->h : s->h);
    if (w <= 0 || h <= 0)
        return 0;
    for (y = 0; y < h; y++)
        memcpy(MEM + d->data + (size_t)y * d->w * 4u,
               (const void *)(MEM + s->data + (size_t)y * s->w * 4u),
               (size_t)w * 4u);
    return 1;
}

uint32_t surface_create(uint32_t w, uint32_t h, uint32_t format, int renderable)
{
    uint32_t obj = com_create_named("IDirect3DSurface9", surface_methods,
                              (int)(sizeof(surface_methods) / sizeof(method_t)),
                              sizeof(surface_t_));
    surface_t_ *s = surf(obj);
    s->w = w;
    s->h = h;
    s->format = format;
    s->renderable = renderable;
    s->data = emu_alloc(w * h * 4 + 64);
    return obj;
}

/* -------------------------------------------------------------- shaders */
static void sh_QueryInterface(void) { RET32(0); }
static void sh_AddRef(void) { M32(arg32(0) + 4)++; RET32(M32(arg32(0) + 4)); }
static void sh_Release(void) { RET32(0); }
static void sh_GetDevice(void) { RET32(0); }
static void sh_GetFunction(void)
{
    uint32_t pp = arg32(1), ps = arg32(2);
    shader_t *s = shad(arg32(0));
    uint32_t n = M32(s->code + 4) * 4u;
    if (pp)
        M32(pp) = s->code;
    if (ps)
        M32(ps) = n;
    RET32(0);
}

static const method_t shader_methods[] = {
    {sh_QueryInterface, 3},   /* QueryInterface */
    {sh_AddRef, 1},   /* AddRef */
    {sh_Release, 1},   /* Release */
    {sh_GetDevice, 2},   /* GetDevice */
    {sh_GetFunction, 3},   /* GetFunction */
};

uint32_t shader_create(uint32_t code_addr, int pixel)
{
    uint32_t obj = com_create_named("IDirect3DVertexShader9", shader_methods, 5, sizeof(shader_t));
    shader_t *s = shad(obj);
    s->code = code_addr;
    s->pixel = pixel;
    uint32_t version = code_addr ? M32(code_addr) : 0;
    log_printf("d3d9: %s shader %08x version %08x", pixel ? "pixel" : "vertex",
               obj, version);
    return obj;
}

uint32_t texture_data_of(uint32_t o) { return tex(o)->data; }

uint32_t texture_width(uint32_t o) { return tex(o)->w; }
uint32_t texture_height(uint32_t o) { return tex(o)->h; }

/* ------------------------------------------------------- texture decoding
 * Textures arrive as raw A8R8G8B8 ("FMT_ARGB") or as compressed DXT1/3/5
 * blocks (that is what decode_dds() records - it only parses the header).  The
 * software rasteriser needs linear ARGB, so decode on first use and keep the
 * result in a small side cache keyed by the emulated texture object. */
enum { TF_ARGB = 0, TF_DXT1, TF_DXT3, TF_DXT5, TF_L8, TF_A8,
       TF_A4R4G4B4, TF_A1R5G5B5, TF_R5G6B5,
       TF_R8G8B8, TF_X8R8G8B8, TF_X1R5G5B5 };

#define TEX_CACHE 64
static struct {
    uint32_t obj, w, h;
    uint32_t *px;
} g_tex[64];

static uint32_t *tex_cache_slot(uint32_t obj, uint32_t w, uint32_t h)
{
    static unsigned next;
    for (int i = 0; i < TEX_CACHE; i++)
        if (g_tex[i].px && g_tex[i].obj == obj && g_tex[i].w == w &&
            g_tex[i].h == h)
            return g_tex[i].px;
    for (int i = 0; i < TEX_CACHE; i++) {
        if (!g_tex[i].px) {
            g_tex[i].px = (uint32_t *)malloc((size_t)w * h * 4);
            g_tex[i].obj = obj;
            g_tex[i].w = w;
            g_tex[i].h = h;
            return g_tex[i].px;
        }
    }
    /* all slots busy: recycle */
    unsigned s = next++ % TEX_CACHE;
    free(g_tex[s].px);
    g_tex[s].px = (uint32_t *)malloc((size_t)w * h * 4);
    g_tex[s].obj = obj;
    g_tex[s].w = w;
    g_tex[s].h = h;
    return g_tex[s].px;
}

static uint32_t rgb565(uint32_t c)
{
    uint32_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* one 4x4 colour block (8 bytes) */
static uint32_t mix2(uint32_t a, uint32_t b)   /* (a + b) / 2 per channel */
{
    uint32_t r = (((a >> 16) & 255) + ((b >> 16) & 255)) >> 1;
    uint32_t g = (((a >> 8) & 255) + ((b >> 8) & 255)) >> 1;
    uint32_t bl = ((a & 255) + (b & 255)) >> 1;
    return 0xFF000000u | (r << 16) | (g << 8) | bl;
}

static uint32_t mix3(uint32_t a, uint32_t b)   /* (2a + b) / 3 per channel */
{
    uint32_t r = (2 * ((a >> 16) & 255) + ((b >> 16) & 255)) / 3;
    uint32_t g = (2 * ((a >> 8) & 255) + ((b >> 8) & 255)) / 3;
    uint32_t bl = (2 * (a & 255) + (b & 255)) / 3;
    return 0xFF000000u | (r << 16) | (g << 8) | bl;
}

static void dxt_color_block(const uint8_t *b, uint32_t *out, int stride,
                            int force4, uint32_t alpha_or)
{
    uint32_t c0 = (uint32_t)b[0] | ((uint32_t)b[1] << 8);
    uint32_t c1 = (uint32_t)b[2] | ((uint32_t)b[3] << 8);
    uint32_t p[4];
    p[0] = rgb565(c0) | alpha_or;
    p[1] = rgb565(c1) | alpha_or;
    /* The 1/3- and 2/3-blend colours must be interpolated per channel: doing
     * the arithmetic on the packed 0xRRGGBB value divided as one 24-bit
     * integer makes the three channels carry into each other, which showed up
     * as coloured speckles in every DXT1/3/5 texture (the p02_* atlases). */
    p[2] = mix2(p[0], p[1]);
    if (force4 || c0 > c1) {
        p[3] = mix3(p[0], p[1]);
    } else {
        p[3] = 0;   /* transparent black */
    }
    uint32_t bits = (uint32_t)b[4] | ((uint32_t)b[5] << 8) |
                    ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++, bits >>= 2)
            out[y * stride + x] = p[bits & 3];
}

static uint32_t *decode_dxt(uint32_t obj, uint32_t w, uint32_t h, int fmt)
{
    const uint8_t *src = MEM + tex(obj)->data;
    uint32_t *dst, *cached = tex_cache_slot(obj, w, h);
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    uint32_t bs = (fmt == TF_DXT1) ? 8u : 16u;

    if (!cached)
        return NULL;
    dst = cached;
    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t *blk = src + ((size_t)by * bw + bx) * bs;
            uint32_t px[16];
            uint32_t alpha_or = 0;
            if (fmt == TF_DXT3) {
                uint64_t abits = 0;
                for (int i = 0; i < 8; i++)
                    abits |= (uint64_t)blk[i] << (i * 8);
                dxt_color_block(blk + 8, px, 4, 1, 0);
                for (int i = 0; i < 16; i++) {
                    uint32_t a = (uint32_t)((abits >> (i * 4)) & 0xF);
                    px[i] = (px[i] & 0x00FFFFFFu) | ((a * 17u) << 24);
                }
            } else if (fmt == TF_DXT5) {
                uint32_t a0 = blk[0], a1 = blk[1];
                uint64_t abits = 0;
                for (int i = 0; i < 6; i++)
                    abits |= (uint64_t)blk[2 + i] << (i * 8);
                uint32_t al[8];
                al[0] = a0;
                al[1] = a1;
                if (a0 > a1) {
                    for (int i = 1; i <= 6; i++)
                        al[i + 1] = ((7 - i) * a0 + i * a1) / 7;
                } else {
                    for (int i = 1; i <= 4; i++)
                        al[i + 1] = ((5 - i) * a0 + i * a1) / 5;
                    al[6] = 0;
                    al[7] = 255;
                }
                dxt_color_block(blk + 8, px, 4, 1, 0);
                for (int i = 0; i < 16; i++) {
                    uint32_t a = al[(abits >> (i * 3)) & 7];
                    px[i] = (px[i] & 0x00FFFFFFu) | (a << 24);
                }
            } else {
                dxt_color_block(blk, px, 4, 0, 0xFF000000u);
            }
            for (uint32_t y = 0; y < 4; y++) {
                uint32_t dy = by * 4 + y;
                if (dy >= h)
                    break;
                for (uint32_t x = 0; x < 4; x++) {
                    uint32_t dx = bx * 4 + x;
                    if (dx >= w)
                        break;
                    dst[(size_t)dy * w + dx] = px[y * 4 + x];
                }
            }
        }
    }
    return dst;
}

/* Linear 0xAARRGGBB pixels for a texture object (NULL if it has no data). */
const uint32_t *texture_argb(uint32_t obj, uint32_t *w, uint32_t *h)
{
    texture_t *t;
    if (!obj)
        return NULL;
    t = tex(obj);
    if (!t || !t->w || !t->h || !t->data)
        return NULL;
    *w = t->w;
    *h = t->h;
    switch (t->fmt) {
    case TF_ARGB:
        return (const uint32_t *)(MEM + t->data);
    case TF_L8: {
        uint32_t *px = tex_cache_slot(obj, t->w, t->h);
        const uint8_t *s = MEM + t->data;
        if (!px)
            return NULL;
        for (uint32_t i = 0; i < t->w * t->h; i++) {
            uint32_t v = s[i];
            px[i] = 0xFF000000u | (v << 16) | (v << 8) | v;
        }
        return px;
    }
    case TF_A8: {
        uint32_t *px = tex_cache_slot(obj, t->w, t->h);
        const uint8_t *s = MEM + t->data;
        if (!px)
            return NULL;
        for (uint32_t i = 0; i < t->w * t->h; i++)
            px[i] = (uint32_t)s[i] << 24;
        return px;
    }
    case TF_A4R4G4B4: {
        uint32_t *px = tex_cache_slot(obj, t->w, t->h);
        const uint8_t *s8 = MEM + t->data;
        if (!px)
            return NULL;
        for (uint32_t i = 0; i < t->w * t->h; i++) {
            uint32_t v = (uint32_t)s8[i * 2] | ((uint32_t)s8[i * 2 + 1] << 8);
            uint32_t r = (v >> 8) & 0xF, g = (v >> 4) & 0xF;
            uint32_t b = v & 0xF, a = (v >> 12) & 0xF;
            px[i] = ((a * 17u) << 24) | ((r * 17u) << 16) |
                    ((g * 17u) << 8) | (b * 17u);
        }
        return px;
    }
    case TF_A1R5G5B5:
    case TF_X1R5G5B5:
    case TF_R5G6B5: {
        uint32_t *px = tex_cache_slot(obj, t->w, t->h);
        const uint8_t *s8 = MEM + t->data;
        int fmt = (int)t->fmt;
        if (!px)
            return NULL;
        for (uint32_t i = 0; i < t->w * t->h; i++) {
            uint32_t v = (uint32_t)s8[i * 2] | ((uint32_t)s8[i * 2 + 1] << 8);
            uint32_t r, g, b, a;
            if (fmt == TF_R5G6B5) {          /* 5-6-5, no alpha */
                r = (v >> 11) & 0x1F;
                g = (v >> 5) & 0x3F;
                b = v & 0x1F;
                a = 0xFF;
                px[i] = (a << 24) | ((r * 255u / 31u) << 16) |
                        ((g * 255u / 63u) << 8) | (b * 255u / 31u);
            } else {                          /* A1R5G5B5 / X1R5G5B5 */
                r = (v >> 10) & 0x1F;
                g = (v >> 5) & 0x1F;
                b = v & 0x1F;
                a = fmt == TF_A1R5G5B5 ? ((v & 0x8000u) ? 0xFFu : 0u)
                                       : 0xFFu;
                px[i] = (a << 24) | ((r * 255u / 31u) << 16) |
                        ((g * 255u / 31u) << 8) | (b * 255u / 31u);
            }
        }
        return px;
    }
    case TF_R8G8B8: {
        uint32_t *px = tex_cache_slot(obj, t->w, t->h);
        const uint8_t *s8 = MEM + t->data;
        if (!px)
            return NULL;
        for (uint32_t i = 0; i < t->w * t->h; i++) {
            const uint8_t *p = s8 + (size_t)i * 3u;
            px[i] = 0xFF000000u | ((uint32_t)p[2] << 16) |
                    ((uint32_t)p[1] << 8) | p[0];
        }
        return px;
    }
    case TF_X8R8G8B8: {
        uint32_t *px = tex_cache_slot(obj, t->w, t->h);
        const uint32_t *s32 = (const uint32_t *)(MEM + t->data);
        if (!px)
            return NULL;
        for (uint32_t i = 0; i < t->w * t->h; i++)
            px[i] = s32[i] | 0xFF000000u;
        return px;
    }
    case TF_DXT1:
    case TF_DXT3:
    case TF_DXT5:
        return decode_dxt(obj, t->w, t->h, t->fmt);
    default:
        return (const uint32_t *)(MEM + t->data);
    }
}