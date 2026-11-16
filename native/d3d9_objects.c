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

static void t_GetLevelDesc(void) { RET32(0); }
static void t_LockRect(void) { RET32(0x8876086Au); }
static void t_UnlockRect(void) { RET32(0); }
static void t_AddDirtyRect(void) { RET32(0); }

static void t_GetSurfaceLevel(void)
{
    uint32_t level = arg32(1), pp = arg32(2);
    if (pp)
        M32(pp) = surface_create(tex(arg32(0))->w, tex(arg32(0))->h,
                                 tex(arg32(0))->format, 1);
    (void)level;
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
    log_printf("d3d9: texture %08x %ux%u uploaded (%u bytes, fmt %d)", obj, t->w,
               t->h, size, fmt);
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
static void s_GetDesc(void) { RET32(0); }
static void s_LockRect(void)
{
    uint32_t pp = arg32(3);
    if (pp)
        M32(pp) = surf(arg32(0))->data;
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
