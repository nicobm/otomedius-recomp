/* d3d9.c - Direct3D 9 objects backed by the native renderer.
 *
 * The game is a Direct3D 9 title: it creates one device, a pile of textures,
 * vertex buffers, shaders and a D3DX effect, then draws every frame.  This
 * file owns the COM objects (they live in emulated memory so the game can
 * call them through the original vtables) and forwards the work to the
 * renderer in render.c.
 */

#define _GNU_SOURCE
#include "win32.h"
#include "d3d9.h"
#include "platform.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* sizeof(D3DCAPS9) */
#define D3DCAPS9_FIELDS 212u

/* ------------------------------------------------------------ COM helper */
static uint32_t g_obj_bump = 0x60000000u;

uint32_t emu_alloc(uint32_t size)
{
    size = (size + 15u) & ~15u;
    uint32_t p = g_obj_bump;
    g_obj_bump += size;
    memset(MEM + p, 0, size);
    return p;
}

#define MAX_VT 64
static struct {
    uint32_t vt;
    uint32_t count;
    const char *name;
    const method_t *methods;
} g_vt[MAX_VT];
static unsigned g_nvt;

/* Every COM object of the same class shares one vtable.  The original code
 * registered a fresh set of native thunks for every object; a title that
 * creates a texture per frame exhausted the fixed 0x10000-entry thunk table
 * ("imports: too many native thunks") after a few thousand frames. */
static uint32_t vtable_for(const char *name, const method_t *methods, int count)
{
    for (unsigned v = 0; v < g_nvt; v++)
        if (g_vt[v].methods == methods && g_vt[v].count == (uint32_t)count)
            return g_vt[v].vt;
    uint32_t vt = emu_alloc((uint32_t)count * 4u);
    for (int i = 0; i < count; i++)
        M32(vt + (uint32_t)i * 4u) = x86_native_register(methods[i].fn,
                                                         methods[i].nargs);
    if (g_nvt < MAX_VT) {
        g_vt[g_nvt].vt = vt;
        g_vt[g_nvt].count = (uint32_t)count;
        g_vt[g_nvt].name = name;
        g_vt[g_nvt].methods = methods;
        g_nvt++;
    }
    return vt;
}

uint32_t com_create_named(const char *name, const method_t *methods, int count,
                          uint32_t extra)
{
    uint32_t vt = vtable_for(name, methods, count);
    uint32_t obj = emu_alloc(8u + extra);
    M32(obj) = vt;
    M32(obj + 4) = 1;
    return obj;
}

/* Map a native thunk back to "Interface[index]" using the vtable we built. */
const char *com_method_name(uint32_t thunk)
{
    static char buf[96];
    for (unsigned v = 0; v < g_nvt; v++) {
        for (uint32_t i = 0; i < g_vt[v].count; i++) {
            /* thunks are allocated in registration order and 16 bytes apart */
            if (M32(g_vt[v].vt + i * 4) == thunk) {
                snprintf(buf, sizeof(buf), "%s[%u]", g_vt[v].name, i);
                return buf;
            }
        }
    }
    return NULL;
}

uint32_t com_create(const method_t *methods, int count, uint32_t extra)
{
    return com_create_named("COM", methods, count, extra);
}

uint32_t com_create_unused(const method_t *methods, int count, uint32_t extra)
{
    uint32_t vt = emu_alloc((uint32_t)count * 4u);
    for (int i = 0; i < count; i++)
        M32(vt + (uint32_t)i * 4u) = x86_native_register(methods[i].fn, methods[i].nargs);
    uint32_t obj = emu_alloc(8u + extra);
    M32(obj) = vt;
    M32(obj + 4) = 1;
    return obj;
}

uint32_t com_vtable(uint32_t obj) { return M32(obj); }
uint32_t com_refcount(uint32_t obj) { return M32(obj + 4); }

/* --------------------------------------------------------- shared state */
typedef struct {
    uint32_t obj;
    int type;
    uint32_t w, h, levels, usage, format;
    int is_rt;
} surface_t;

typedef struct {
    uint32_t obj;
    uint32_t size, usage, fvf, format;
} resource_t;

static struct {
    int present;
    uint32_t width, height;
    uint32_t clear_color;
    int scene;
} g_dev;

/* ------------------------------------------------------------ IDirect3D9 */
static void d9_QueryInterface(void) { RET32(0); }
static void d9_AddRef(void) { RET32(2); }
static void d9_Release(void) { RET32(0); }

static void d9_RegisterSoftwareDevice(void) { RET32(0x8876086Au); }  /* NOTAVAILABLE */

static void d9_GetAdapterCount(void) { RET32(1); }

static void d9_GetAdapterIdentifier(void)
{
    uint32_t p = arg32(3);      /* (this, Adapter, Flags, pIdentifier) */
    if (p) {
        memset(MEM + p, 0, 152);
        M32(p + 0) = 0x10DE;
        M32(p + 4) = 0x0409;
        memcpy(MEM + p + 8, "Recompiled adapter", 19);
        M32(p + 48) = 0x10000;
        M32(p + 52) = 1;
        M32(p + 56) = 1;
        M32(p + 60) = 0;
    }
    RET32(0);
}

static void d9_GetAdapterModeCount(void) { RET32(1); }

static void d9_EnumAdapterModes(void)
{
    /* (this, Adapter, Format, Mode, pMode).  The game enumerates the adapter
     * modes and picks the one closest to the 1280x1024 it wants (the window
     * constants in .data).  Before a device exists g_dev.width is 0, so the
     * old code advertised 1024x768 and the game created a 1024x768 backbuffer
     * inside a 1280x1024 window: every 2D UI coordinate was then off-screen.
     * Advertise the real desktop/window size instead. */
    uint32_t p = arg32(4);
    if (p) {
        memset(MEM + p, 0, 16);
        M32(p + 0) = g_dev.width ? g_dev.width : (uint32_t)render_width();
        M32(p + 4) = g_dev.height ? g_dev.height : (uint32_t)render_height();
        M32(p + 8) = 60;
        M32(p + 12) = 22;  /* D3DFMT_X8R8G8B8 */
    }
    RET32(0);
}

static void d9_GetAdapterDisplayMode(void)
{
    uint32_t p = arg32(2);
    if (p) {
        memset(MEM + p, 0, 16);
        M32(p + 0) = render_width();
        M32(p + 4) = render_height();
        M32(p + 8) = 60;
        M32(p + 12) = 22;
    }
    RET32(0);
}

static void d9_CheckDeviceType(void) { RET32(0); }
static void d9_CheckDeviceFormat(void) { RET32(0); }
static void d9_CheckDeviceMultiSampleType(void) { RET32(0); }
static void d9_CheckDepthStencilMatch(void) { RET32(0); }
static void d9_CheckDeviceFormatConversion(void) { RET32(0); }

/* A realistic D3DCAPS9 for a 2008 Direct3D 9 GPU: shader model 3.0, plenty of
 * streams, textures and shader constants.  The game refuses to create a
 * device when these fields are zero. */
static void d9_GetDeviceCaps(void)
{
    uint32_t p = arg32(3);      /* (this, Adapter, DeviceType, pCaps) */
    log_printf("d3d9: GetDeviceCaps(this=%08x adapter=%u type=%u caps=%08x)", arg32(0),
               arg32(1), arg32(2), p);
    if (!p)
        return;
    memset(MEM + p, 0, sizeof(D3DCAPS9_FIELDS));
    M32(p + 0) = 1;                 /* DeviceType = D3DDEVTYPE_HAL */
    M32(p + 4) = 0;                 /* AdapterOrdinal */
    M32(p + 8) = 0x00020000;        /* Caps: READ_SCANLINE */
    M32(p + 12) = 0x00000080;       /* Caps2: FULLSCREEN_GAMMA */
    M32(p + 20) = 0x80000000 | 0x40000000 | 0x20000000 | 0x10000000 | 0x8000000;
    M32(p + 24) = 0x00010001;       /* CursorCaps: COLOR, ALPHA */
    M32(p + 28) = 0x00010000 | 0x00020000 | 0x00000020 | 0x00000001; /* DevCaps */
    M32(p + 32) = 0x0000000F;       /* PrimitiveMiscCaps */
    M32(p + 36) = 0x00010000 | 0x00000001 | 0x00000080;  /* RasterCaps */
    M32(p + 40) = 0x000000FF;       /* ZCmpCaps */
    M32(p + 44) = 0x00001000 | 0x0000000F;  /* SrcBlendCaps */
    M32(p + 48) = 0x00001000 | 0x0000000F;  /* DestBlendCaps */
    M32(p + 52) = 0x000000FF;       /* AlphaCmpCaps */
    M32(p + 56) = 0x00000001 | 0x00000200;  /* ShadeCaps */
    M32(p + 60) = 0x00000010 | 0x00000020 | 0x00000040 | 0x00001000; /* TextureCaps */
    M32(p + 64) = 0x03000000;       /* TextureFilterCaps */
    M32(p + 68) = 0x03000000;       /* CubeTextureFilterCaps */
    M32(p + 72) = 0x07000000;       /* VolumeTextureFilterCaps */
    M32(p + 76) = 0x00003300;       /* TextureAddressCaps */
    M32(p + 80) = 0x00003300;       /* VolumeTextureAddressCaps */
    M32(p + 84) = 0x001F0000;       /* LineCaps */
    M32(p + 88) = 8192;             /* MaxTextureWidth */
    M32(p + 92) = 8192;             /* MaxTextureHeight */
    M32(p + 96) = 2048;             /* MaxVolumeExtent */
    M32(p + 100) = 8192;            /* MaxTextureRepeat */
    M32(p + 104) = 8192;            /* MaxTextureAspectRatio */
    M32(p + 108) = 16;              /* MaxAnisotropy */
    MF32(p + 112) = 1.0f;           /* MaxVertexW */
    MF32(p + 116) = -32768.0f;
    MF32(p + 120) = -32768.0f;
    MF32(p + 124) = 32768.0f;
    MF32(p + 128) = 32768.0f;
    MF32(p + 132) = 0.0f;
    M32(p + 136) = 0x000000FF;      /* StencilCaps */
    M32(p + 140) = 0x00100000 | 0x00000020; /* FVFCaps */
    M32(p + 144) = 0x0041FFFF;      /* TextureOpCaps */
    M32(p + 148) = 8;               /* MaxTextureBlendStages */
    M32(p + 152) = 8;               /* MaxSimultaneousTextures */
    M32(p + 156) = 0x00000031;      /* VertexProcessingCaps */
    M32(p + 160) = 8;               /* MaxActiveLights */
    M32(p + 164) = 6;               /* MaxUserClipPlanes */
    M32(p + 168) = 4;               /* MaxVertexBlendMatrices */
    M32(p + 172) = 0;               /* MaxVertexBlendMatrixIndex */
    MF32(p + 176) = 64.0f;          /* MaxPointSize */
    M32(p + 180) = 0x007FFFFF;      /* MaxPrimitiveCount */
    M32(p + 184) = 0x00FFFFFF;      /* MaxVertexIndex */
    M32(p + 188) = 16;              /* MaxStreams */
    M32(p + 192) = 255;             /* MaxStreamStride */
    M32(p + 196) = 0xFFFE0300;      /* VertexShaderVersion: vs_3_0 */
    M32(p + 200) = 256;             /* MaxVertexShaderConst */
    M32(p + 204) = 0xFFFF0300;      /* PixelShaderVersion: ps_3_0 */
    MF32(p + 208) = 1.0f;           /* MaxPixelShaderValue */
    RET32(0);
}

static void d9_GetDeviceCaps2(void) { RET32(0); }

static void d9_GetAdapterMonitor(void) { RET32(0x00010001u); }

static void d9_CreateDevice(void)
{
    uint32_t pp = arg32(5), ppdev = arg32(6);
    uint32_t w = pp ? M32(pp + 0) : 0, h = pp ? M32(pp + 4) : 0;
    log_printf("d3d9: CreateDevice(%ux%u) windowed=%u", w, h,
               pp ? M32(pp + 32) : 0);
    uint32_t dev = device_create(w, h);
    if (ppdev)
        M32(ppdev) = dev;
    RET32(0);
}

static const method_t d3d9_methods[] = {
    {d9_QueryInterface, 3},   /* QueryInterface */
    {d9_AddRef, 1},   /* AddRef */
    {d9_Release, 1},   /* Release */
    {d9_RegisterSoftwareDevice, 2},   /* RegisterSoftwareDevice */
    {d9_GetAdapterCount, 1},   /* GetAdapterCount */
    {d9_GetAdapterIdentifier, 4},   /* GetAdapterIdentifier */
    {d9_GetAdapterModeCount, 3},   /* GetAdapterModeCount */
    {d9_EnumAdapterModes, 5},   /* EnumAdapterModes */
    {d9_GetAdapterDisplayMode, 3},   /* GetAdapterDisplayMode */
    {d9_CheckDeviceType, 6},   /* CheckDeviceType */
    {d9_CheckDeviceFormat, 7},   /* CheckDeviceFormat */
    {d9_CheckDeviceMultiSampleType, 7},   /* CheckDeviceMultiSampleType */
    {d9_CheckDepthStencilMatch, 6},   /* CheckDepthStencilMatch */
    {d9_CheckDeviceFormatConversion, 5},   /* CheckDeviceFormatConversion */
    {d9_GetDeviceCaps, 4},   /* GetDeviceCaps */
    {d9_GetAdapterMonitor, 2},   /* GetAdapterMonitor */
    {d9_CreateDevice, 7},   /* CreateDevice */
};

/* ------------------------------------------------------ IDirect3DDevice9 */
typedef struct {
    uint32_t vtable[8];
    uint32_t textures[16];
    uint32_t viewport[6];
    uint32_t rt;            /* current render target */
} device_t;

static device_t *dev(uint32_t obj) { return (device_t *)(MEM + obj); }

static void dev_QueryInterface(void) { RET32(0); }
static void dev_AddRef(void) { RET32(2); }
static void dev_Release(void) { RET32(0); }

static void dev_TestCooperativeLevel(void) { RET32(0); }
static void dev_GetAvailableTextureMem(void) { RET32(64u * 1024u * 1024u); }

static void dev_EvictManagedResources(void) { RET32(0); }
static void dev_GetDirect3D(void) { RET32(0); }
static void dev_GetDeviceCaps(void) { RET32(0); }
static void dev_GetDisplayMode(void) { RET32(0); }
static void dev_GetCreationParameters(void) { RET32(0); }

static void dev_SetCursorProperties(void) { RET32(0); }
static void dev_SetCursorPosition(void) { RET32(0); }
static void dev_ShowCursor(void) { RET32(0); }

static void dev_CreateAdditionalSwapChain(void) { RET32(0x8876086Cu); }
static void dev_GetSwapChain(void) { RET32(0); }
static void dev_GetNumberOfSwapChains(void) { RET32(1); }

static void dev_Reset(void)
{
    log_printf("d3d9: Reset");
    RET32(0);
}

static void dev_Present(void)
{
    render_present();
    RET32(0);
}

static void dev_GetBackBuffer(void)
{
    /* The scene renderer asks for the back buffer, reads its description and
     * creates a matching texture (the 3D target).  Returning S_OK with a NULL
     * surface made the caller dereference address 0. */
    uint32_t pp = arg32(4);
    static uint32_t surface;
    if (!surface)
        surface = surface_create((uint32_t)render_width(), (uint32_t)render_height(),
                                 0x15u /* D3DFMT_A8R8G8B8 */, 1);
    /* Alias the renderer's framebuffer: the game reads the 3D scene back
     * through this surface (GetRenderTargetData into its scene texture). */
    surface_alias_backbuffer(surface);
    if (pp)
        M32(pp) = surface;
    RET32(0);
}

static void dev_GetRasterStatus(void) { RET32(0); }

static void dev_SetDialogBoxMode(void) { RET32(0); }

static void dev_SetGammaRamp(void) { RET32(0); }
static void dev_GetGammaRamp(void) { RET32(0); }

static void dev_CreateTexture(void)
{
    uint32_t w = arg32(1), h = arg32(2), levels = arg32(3), usage = arg32(4),
             format = arg32(5), pool = arg32(6), pp = arg32(7);
    uint32_t tex = texture_create(w, h, levels, usage, format, pool);
    if (pp)
        M32(pp) = tex;
    if (getenv("OTOMEDIUS_TEXDBG")) {
        static unsigned n;
        if (n++ < 60)
            fprintf(stderr, "d3d9: CreateTexture %ux%u lv=%u usage=%x fmt=%x "
                    "pool=%u -> %08x pp=%08x<%08x> ret=%08x\n",
                    w, h, levels, usage, format, pool, tex, pp,
                    pp ? M32(pp) : 0, M32(x86_cpu()->esp));
    }
    RET32(0);
}

static void dev_CreateVolumeTexture(void) { RET32(0x8876086Au); }
static void dev_CreateCubeTexture(void) { RET32(0x8876086Au); }

static void dev_CreateVertexBuffer(void)
{
    uint32_t size = arg32(1), usage = arg32(2), fvf = arg32(3), pool = arg32(4), pp = arg32(6);
    uint32_t vb = buffer_create(size, usage, fvf, 0);
    if (pp)
        M32(pp) = vb;
    RET32(0);
}

static void dev_CreateIndexBuffer(void)
{
    uint32_t size = arg32(1), usage = arg32(2), fmt = arg32(3), pool = arg32(4), pp = arg32(6);
    uint32_t ib = buffer_create(size, usage, fmt, 1);
    if (pp)
        M32(pp) = ib;
    RET32(0);
}

static void dev_CreateRenderTarget(void)
{
    uint32_t w = arg32(1), h = arg32(2), fmt = arg32(3), pp = arg32(6);
    uint32_t s = surface_create(w, h, fmt, 1);
    if (pp)
        M32(pp) = s;
    RET32(0);
}

static void dev_CreateDepthStencilSurface(void)
{
    uint32_t w = arg32(1), h = arg32(2), fmt = arg32(3), pp = arg32(6);
    uint32_t s = surface_create(w, h, fmt, 0);
    if (pp)
        M32(pp) = s;
    RET32(0);
}

static void dev_UpdateSurface(void)
{
    static unsigned n;
    if (n++ < 3)
        fprintf(stderr, "d3d9: UNIMPLEMENTED dev_UpdateSurface called (#%u) args %08x %08x %08x %08x\n", n,
                arg32(1), arg32(2), arg32(3), arg32(4));
    RET32(0);
}
static void dev_UpdateTexture(void) { RET32(0); }
/* GetRenderTargetData(pRenderTarget, pDestSurface): the scene texture really
 * receives the framebuffer contents here; the game later composites it as the
 * background of the 2D HUD. */
static void dev_GetRenderTargetData(void)
{
    uint32_t dst = arg32(2), src = arg32(1);
    static unsigned n, ok;
    if (src && dst) {
        if (surface_copy(dst, src))
            ok++;
        else if (n++ < 3)
            fprintf(stderr, "d3d9: GetRenderTargetData copy failed src=%08x dst=%08x\n",
                    src, dst);
    }
    if (getenv("OTOMEDIUS_RTDBG"))
        fprintf(stderr, "d3d9: GetRenderTargetData #%u src=%08x dst=%08x ok=%u\n",
                n + ok, src, dst, ok);
    RET32(0);
}
static void dev_GetFrontBufferData(void) { RET32(0); }
static void dev_StretchRect(void)
{
    /* StretchRect(src, pSrcRect, dst, pDstRect, Filter): only the
     * same-size copy is needed (back buffer -> scene texture). */
    uint32_t src = arg32(1), dst = arg32(3);
    static unsigned n;
    if (src && dst) {
        if (!surface_copy(dst, src) && n++ < 3)
            fprintf(stderr, "d3d9: StretchRect copy failed src=%08x dst=%08x\n",
                    src, dst);
    }
    RET32(0);
}
static void dev_ColorFill(void)
{
    static unsigned n;
    if (n++ < 3)
        fprintf(stderr, "d3d9: UNIMPLEMENTED dev_ColorFill called (#%u) args %08x %08x %08x %08x\n", n,
                arg32(1), arg32(2), arg32(3), arg32(4));
    RET32(0);
}

static void dev_CreateOffscreenPlainSurface(void)
{
    uint32_t w = arg32(1), h = arg32(2), fmt = arg32(3), pp = arg32(5);
    uint32_t s = surface_create(w, h, fmt, 1);
    if (pp)
        M32(pp) = s;
    RET32(0);
}

/* The game renders the 3D scene into a scene texture and then composites it
 * under the 2D HUD, so the target has to be real: Clear and Draw* are
 * redirected to the surface's pixel buffer. */
static void dev_SetRenderTarget(void)
{
    uint32_t idx = arg32(1), s = arg32(2);
    if (idx == 0) {
        dev(arg32(0))->rt = s;
        if (s) {
            uint32_t px = surface_pixels(s);
            int w = surface_width(s), h = surface_height(s);
            if (getenv("OTOMEDIUS_RTDBG"))
                fprintf(stderr, "d3d9: SetRenderTarget surface=%08x %dx%d "
                        "pixels=%08x\n", s, w, h, px);
            render_set_target(px, w, h);
        } else {
            render_set_target(0, 0, 0);
        }
    }
    RET32(0);
}

static void dev_GetRenderTarget(void)
{
    uint32_t pp = arg32(2);
    if (pp)
        M32(pp) = dev(arg32(0))->rt;
    RET32(0);
}

static void dev_SetDepthStencilSurface(void) { RET32(0); }
static void dev_GetDepthStencilSurface(void) { RET32(0); }

static void dev_BeginScene(void)
{
    g_dev.scene = 1;
    RET32(0);
}

static void dev_EndScene(void)
{
    g_dev.scene = 0;
    RET32(0);
}

static void dev_Clear(void)
{
    int count = (int)arg32(1);
    uint32_t rects = arg32(2), flags = arg32(3), color = arg32(4);
    render_clear(count, rects, flags, color);
    (void)color;
    RET32(0);
}

static void dev_SetTransform(void)
{
    uint32_t state = arg32(1), m = arg32(2);
    if ((g_verbose > 1 || getenv("OTOMEDIUS_VPDBG")) && m) {
        static int n;
        if (n++ < 40) {
            const float *f = (const float *)(MEM + m);
            fprintf(stderr, "d3d9: SetTransform(state=%u) "
                    "[%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f]\n",
                    state, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
        }
    }
    if (m)
        render_set_transform(state, MEM + m);
    RET32(0);
}

static void dev_GetTransform(void)
{
    uint32_t state = arg32(1), m = arg32(2);
    extern int render_get_transform(uint32_t state, void *out);
    if (m)
        render_get_transform(state, MEM + m);
    RET32(0);
}

static void dev_MultiplyTransform(void) { RET32(0); }

static void dev_SetViewport(void)
{
    uint32_t p = arg32(1);
    uint32_t *vp = dev(arg32(0))->viewport;
    if (p) {
        vp[0] = M32(p + 0);
        vp[1] = M32(p + 4);
        vp[2] = M32(p + 8);
        vp[3] = M32(p + 12);
        vp[4] = M32(p + 16);
        vp[5] = M32(p + 20);
        render_set_viewport(vp[0], vp[1], vp[2], vp[3]);
        if (g_verbose > 1 || getenv("OTOMEDIUS_VPDBG"))
            fprintf(stderr, "d3d9: SetViewport %u,%u %ux%u minz=%g maxz=%g\n",
                    vp[0], vp[1], vp[2], vp[3],
                    *(float *)&vp[4], *(float *)&vp[5]);
    }
    RET32(0);
}

static void dev_GetViewport(void)
{
    uint32_t p = arg32(1);
    if (p) {
        uint32_t *vp = dev(arg32(0))->viewport;
        for (int i = 0; i < 6; i++)
            M32(p + (uint32_t)i * 4) = vp[i];
    }
    RET32(0);
}

static void dev_SetMaterial(void) { RET32(0); }
static void dev_GetMaterial(void) { RET32(0); }
static void dev_SetLight(void) { RET32(0); }
static void dev_GetLight(void) { RET32(0); }
static void dev_LightEnable(void) { RET32(0); }
static void dev_GetLightEnable(void) { RET32(0); }
static void dev_SetClipPlane(void) { RET32(0); }
static void dev_GetClipPlane(void) { RET32(0); }

static void dev_SetRenderState(void)
{
    render_set_state(arg32(1), arg32(2));
    RET32(0);
}

static void dev_GetRenderState(void) { RET32(0); }

static void dev_CreateStateBlock(void) { RET32(0); }
static void dev_BeginStateBlock(void) { RET32(0); }
static void dev_EndStateBlock(void) { RET32(0); }
static void dev_SetClipStatus(void) { RET32(0); }
static void dev_GetClipStatus(void) { RET32(0); }

static void dev_GetTexture(void) { RET32(0); }

static void dev_SetTexture(void)
{
    uint32_t stage = arg32(1), tex = arg32(2);
    if (stage < 16)
        dev(arg32(0))->textures[stage] = tex;
    render_set_texture(stage, tex);
    RET32(0);
}

static void dev_GetTextureStageState(void) { RET32(0); }
static void dev_SetTextureStageState(void)
{
    {
        extern void render_set_texture_stage_state(uint32_t, uint32_t, uint32_t);
        render_set_texture_stage_state(arg32(1), arg32(2), arg32(3));
    }
    if (g_verbose > 1) {
        static int n;
        if (n++ < 24)
            fprintf(stderr, "d3d9: SetTextureStageState(stage=%u type=%u value=%u)\n",
                    arg32(1), arg32(2), arg32(3));
    }
    RET32(0);
}
static void dev_GetSamplerState(void) { RET32(0); }

static void dev_SetSamplerState(void)
{
    render_set_sampler(arg32(1), arg32(2), arg32(3));
    RET32(0);
}

static void dev_ValidateDevice(void) { RET32(0); }

static void dev_SetPaletteEntries(void) { RET32(0); }
static void dev_GetPaletteEntries(void) { RET32(0); }
static void dev_SetCurrentTexturePalette(void) { RET32(0); }
static void dev_GetCurrentTexturePalette(void) { RET32(0); }

static void dev_SetScissorRect(void)
{
    static unsigned n;
    if (n++ < 3)
        fprintf(stderr, "d3d9: UNIMPLEMENTED dev_SetScissorRect called (#%u) args %08x %08x %08x %08x\n", n,
                arg32(1), arg32(2), arg32(3), arg32(4));
    RET32(0);
}
static void dev_GetScissorRect(void) { RET32(0); }

static void dev_SetSoftwareVertexProcessing(void) { RET32(0); }
static void dev_GetSoftwareVertexProcessing(void) { RET32(0); }
static void dev_SetNPatchMode(void) { RET32(0); }
static void dev_GetNPatchMode(void) { RET32(0); }

static void dev_DrawPrimitive(void)
{
    render_draw(arg32(1), 0, 0, arg32(2), 0, 0, arg32(3));
    RET32(0);
}

static void dev_DrawIndexedPrimitive(void)
{
    render_draw(arg32(1), arg32(3), arg32(4), arg32(5), arg32(6), 1, arg32(2));
    RET32(0);
}

static void dev_DrawPrimitiveUP(void)
{
    render_draw_up(arg32(1), arg32(2), arg32(3), arg32(4), arg32(5));
    RET32(0);
}

static void dev_DrawIndexedPrimitiveUP(void)
{
    static unsigned n;
    if (n++ < 3)
        fprintf(stderr, "d3d9: UNIMPLEMENTED dev_DrawIndexedPrimitiveUP called (#%u) args %08x %08x %08x %08x\n", n,
                arg32(1), arg32(2), arg32(3), arg32(4));
    RET32(0);
}

static void dev_ProcessVertices(void)
{
    static unsigned n;
    if (n++ < 3)
        fprintf(stderr, "d3d9: UNIMPLEMENTED dev_ProcessVertices called (#%u) args %08x %08x %08x %08x\n", n,
                arg32(1), arg32(2), arg32(3), arg32(4));
    RET32(0);
}

static void dev_CreateVertexDeclaration(void) { RET32(0); }
static void dev_SetVertexDeclaration(void) { RET32(0); }
static void dev_GetVertexDeclaration(void) { RET32(0); }

static void dev_SetFVF(void)
{
    render_set_fvf(arg32(1));
    RET32(0);
}
static void dev_GetFVF(void) { RET32(0); }

static void dev_CreateVertexShader(void)
{
    uint32_t code = arg32(1), pp = arg32(2);
    uint32_t sh = shader_create(code, 0);
    if (pp)
        M32(pp) = sh;
    RET32(0);
}

static void dev_SetVertexShader(void) { RET32(0); }
static void dev_GetVertexShader(void) { RET32(0); }
static void dev_SetVertexShaderConstantF(void) { RET32(0); }
static void dev_GetVertexShaderConstantF(void) { RET32(0); }
static void dev_SetVertexShaderConstantI(void) { RET32(0); }
static void dev_GetVertexShaderConstantI(void) { RET32(0); }
static void dev_SetVertexShaderConstantB(void) { RET32(0); }
static void dev_GetVertexShaderConstantB(void) { RET32(0); }

static void dev_SetStreamSource(void)
{
    render_set_stream(arg32(1), arg32(2), arg32(3), arg32(4));
    RET32(0);
}

static void dev_GetStreamSource(void) { RET32(0); }

static void dev_SetStreamSourceFreq(void) { RET32(0); }
static void dev_GetStreamSourceFreq(void) { RET32(0); }

static void dev_SetIndices(void)
{
    render_set_indices(arg32(1), arg32(2));
    RET32(0);
}

static void dev_GetIndices(void) { RET32(0); }

static void dev_CreatePixelShader(void)
{
    uint32_t code = arg32(1), pp = arg32(2);
    uint32_t sh = shader_create(code, 1);
    if (pp)
        M32(pp) = sh;
    RET32(0);
}

static void dev_SetPixelShader(void)
{
    render_set_pixel_shader(arg32(1));
    RET32(0);
}

static void dev_GetPixelShader(void) { RET32(0); }
static void dev_SetPixelShaderConstantF(void) { RET32(0); }
static void dev_GetPixelShaderConstantF(void) { RET32(0); }
static void dev_SetPixelShaderConstantI(void) { RET32(0); }
static void dev_GetPixelShaderConstantI(void) { RET32(0); }
static void dev_SetPixelShaderConstantB(void) { RET32(0); }
static void dev_GetPixelShaderConstantB(void) { RET32(0); }

static void dev_DrawRectPatch(void)
{
    static unsigned n;
    if (n++ < 3)
        fprintf(stderr, "d3d9: UNIMPLEMENTED dev_DrawRectPatch called (#%u) args %08x %08x %08x %08x\n", n,
                arg32(1), arg32(2), arg32(3), arg32(4));
    RET32(0);
}
static void dev_DrawTriPatch(void)
{
    static unsigned n;
    if (n++ < 3)
        fprintf(stderr, "d3d9: UNIMPLEMENTED dev_DrawTriPatch called (#%u) args %08x %08x %08x %08x\n", n,
                arg32(1), arg32(2), arg32(3), arg32(4));
    RET32(0);
}
static void dev_DeletePatch(void) { RET32(0); }

static void dev_CreateQuery(void) { RET32(0x8876086Au); }

static const method_t device_methods[] = {
    {dev_QueryInterface, 3},   /* QueryInterface */
    {dev_AddRef, 1},   /* AddRef */
    {dev_Release, 1},   /* Release */
    {dev_TestCooperativeLevel, 1},   /* TestCooperativeLevel */
    {dev_GetAvailableTextureMem, 1},   /* GetAvailableTextureMem */
    {dev_EvictManagedResources, 1},   /* EvictManagedResources */
    {dev_GetDirect3D, 2},   /* GetDirect3D */
    {dev_GetDeviceCaps, 2},   /* GetDeviceCaps */
    {dev_GetDisplayMode, 3},   /* GetDisplayMode */
    {dev_GetCreationParameters, 2},   /* GetCreationParameters */
    {dev_SetCursorProperties, 4},   /* SetCursorProperties */
    {dev_SetCursorPosition, 4},   /* SetCursorPosition */
    {dev_ShowCursor, 2},   /* ShowCursor */
    {dev_CreateAdditionalSwapChain, 3},   /* CreateAdditionalSwapChain */
    {dev_GetSwapChain, 3},   /* GetSwapChain */
    {dev_GetNumberOfSwapChains, 1},   /* GetNumberOfSwapChains */
    {dev_Reset, 2},   /* Reset */
    {dev_Present, 5},   /* Present */
    {dev_GetBackBuffer, 5},   /* GetBackBuffer */
    {dev_GetRasterStatus, 3},   /* GetRasterStatus */
    {dev_SetDialogBoxMode, 2},   /* SetDialogBoxMode */
    {dev_SetGammaRamp, 4},   /* SetGammaRamp */
    {dev_GetGammaRamp, 3},   /* GetGammaRamp */
    {dev_CreateTexture, 9},   /* CreateTexture */
    {dev_CreateVolumeTexture, 10},   /* CreateVolumeTexture */
    {dev_CreateCubeTexture, 8},   /* CreateCubeTexture */
    {dev_CreateVertexBuffer, 7},   /* CreateVertexBuffer */
    {dev_CreateIndexBuffer, 7},   /* CreateIndexBuffer */
    {dev_CreateRenderTarget, 9},   /* CreateRenderTarget */
    {dev_CreateDepthStencilSurface, 9},   /* CreateDepthStencilSurface */
    {dev_UpdateSurface, 5},   /* UpdateSurface */
    {dev_UpdateTexture, 3},   /* UpdateTexture */
    {dev_GetRenderTargetData, 3},   /* GetRenderTargetData */
    {dev_GetFrontBufferData, 3},   /* GetFrontBufferData */
    {dev_StretchRect, 6},   /* StretchRect */
    {dev_ColorFill, 4},   /* ColorFill */
    {dev_CreateOffscreenPlainSurface, 7},   /* CreateOffscreenPlainSurface */
    {dev_SetRenderTarget, 3},   /* SetRenderTarget */
    {dev_GetRenderTarget, 3},   /* GetRenderTarget */
    {dev_SetDepthStencilSurface, 2},   /* SetDepthStencilSurface */
    {dev_GetDepthStencilSurface, 2},   /* GetDepthStencilSurface */
    {dev_BeginScene, 1},   /* BeginScene */
    {dev_EndScene, 1},   /* EndScene */
    {dev_Clear, 7},   /* Clear */
    {dev_SetTransform, 3},   /* SetTransform */
    {dev_GetTransform, 3},   /* GetTransform */
    {dev_MultiplyTransform, 3},   /* MultiplyTransform */
    {dev_SetViewport, 2},   /* SetViewport */
    {dev_GetViewport, 2},   /* GetViewport */
    {dev_SetMaterial, 2},   /* SetMaterial */
    {dev_GetMaterial, 2},   /* GetMaterial */
    {dev_SetLight, 3},   /* SetLight */
    {dev_GetLight, 3},   /* GetLight */
    {dev_LightEnable, 3},   /* LightEnable */
    {dev_GetLightEnable, 3},   /* GetLightEnable */
    {dev_SetClipPlane, 3},   /* SetClipPlane */
    {dev_GetClipPlane, 3},   /* GetClipPlane */
    {dev_SetRenderState, 3},   /* SetRenderState */
    {dev_GetRenderState, 3},   /* GetRenderState */
    {dev_CreateStateBlock, 3},   /* CreateStateBlock */
    {dev_BeginStateBlock, 1},   /* BeginStateBlock */
    {dev_EndStateBlock, 2},   /* EndStateBlock */
    {dev_SetClipStatus, 2},   /* SetClipStatus */
    {dev_GetClipStatus, 2},   /* GetClipStatus */
    {dev_GetTexture, 3},   /* GetTexture */
    {dev_SetTexture, 3},   /* SetTexture */
    {dev_GetTextureStageState, 4},   /* GetTextureStageState */
    {dev_SetTextureStageState, 4},   /* SetTextureStageState */
    {dev_GetSamplerState, 4},   /* GetSamplerState */
    {dev_SetSamplerState, 4},   /* SetSamplerState */
    {dev_ValidateDevice, 2},   /* ValidateDevice */
    {dev_SetPaletteEntries, 3},   /* SetPaletteEntries */
    {dev_GetPaletteEntries, 3},   /* GetPaletteEntries */
    {dev_SetCurrentTexturePalette, 2},   /* SetCurrentTexturePalette */
    {dev_GetCurrentTexturePalette, 2},   /* GetCurrentTexturePalette */
    {dev_SetScissorRect, 2},   /* SetScissorRect */
    {dev_GetScissorRect, 2},   /* GetScissorRect */
    {dev_SetSoftwareVertexProcessing, 2},   /* SetSoftwareVertexProcessing */
    {dev_GetSoftwareVertexProcessing, 1},   /* GetSoftwareVertexProcessing */
    {dev_SetNPatchMode, 2},   /* SetNPatchMode */
    {dev_GetNPatchMode, 1},   /* GetNPatchMode */
    {dev_DrawPrimitive, 4},   /* DrawPrimitive */
    {dev_DrawIndexedPrimitive, 7},   /* DrawIndexedPrimitive */
    {dev_DrawPrimitiveUP, 5},   /* DrawPrimitiveUP */
    {dev_DrawIndexedPrimitiveUP, 9},   /* DrawIndexedPrimitiveUP */
    {dev_ProcessVertices, 7},   /* ProcessVertices */
    {dev_CreateVertexDeclaration, 3},   /* CreateVertexDeclaration */
    {dev_SetVertexDeclaration, 2},   /* SetVertexDeclaration */
    {dev_GetVertexDeclaration, 2},   /* GetVertexDeclaration */
    {dev_SetFVF, 2},   /* SetFVF */
    {dev_GetFVF, 2},   /* GetFVF */
    {dev_CreateVertexShader, 3},   /* CreateVertexShader */
    {dev_SetVertexShader, 2},   /* SetVertexShader */
    {dev_GetVertexShader, 2},   /* GetVertexShader */
    {dev_SetVertexShaderConstantF, 4},   /* SetVertexShaderConstantF */
    {dev_GetVertexShaderConstantF, 4},   /* GetVertexShaderConstantF */
    {dev_SetVertexShaderConstantI, 4},   /* SetVertexShaderConstantI */
    {dev_GetVertexShaderConstantI, 4},   /* GetVertexShaderConstantI */
    {dev_SetVertexShaderConstantB, 4},   /* SetVertexShaderConstantB */
    {dev_GetVertexShaderConstantB, 4},   /* GetVertexShaderConstantB */
    {dev_SetStreamSource, 5},   /* SetStreamSource */
    {dev_GetStreamSource, 5},   /* GetStreamSource */
    {dev_SetStreamSourceFreq, 3},   /* SetStreamSourceFreq */
    {dev_GetStreamSourceFreq, 3},   /* GetStreamSourceFreq */
    {dev_SetIndices, 2},   /* SetIndices */
    {dev_GetIndices, 2},   /* GetIndices */
    {dev_CreatePixelShader, 3},   /* CreatePixelShader */
    {dev_SetPixelShader, 2},   /* SetPixelShader */
    {dev_GetPixelShader, 2},   /* GetPixelShader */
    {dev_SetPixelShaderConstantF, 4},   /* SetPixelShaderConstantF */
    {dev_GetPixelShaderConstantF, 4},   /* GetPixelShaderConstantF */
    {dev_SetPixelShaderConstantI, 4},   /* SetPixelShaderConstantI */
    {dev_GetPixelShaderConstantI, 4},   /* GetPixelShaderConstantI */
    {dev_SetPixelShaderConstantB, 4},   /* SetPixelShaderConstantB */
    {dev_GetPixelShaderConstantB, 4},   /* GetPixelShaderConstantB */
    {dev_DrawRectPatch, 4},   /* DrawRectPatch */
    {dev_DrawTriPatch, 4},   /* DrawTriPatch */
    {dev_DeletePatch, 2},   /* DeletePatch */
    {dev_CreateQuery, 3},   /* CreateQuery */
};

uint32_t device_create(uint32_t w, uint32_t h)
{
    uint32_t obj = com_create_named("IDirect3DDevice9", device_methods,
                              (int)(sizeof(device_methods) / sizeof(method_t)),
                              sizeof(device_t));
    device_t *d = dev(obj);
    d->viewport[2] = w;
    d->viewport[3] = h;
    d->viewport[4] = w;
    d->viewport[5] = h;
    g_dev.present = 1;
    g_dev.width = w;
    g_dev.height = h;
    render_init(w, h);
    RET32(0);
    return obj;
}

/* --------------------------------------------------------- IDirect3D9 ex */
static void d9c_QueryInterface(void) { RET32(0); }
static void d9c_AddRef(void) { RET32(2); }
static void d9c_Release(void) { RET32(0); }


static void Direct3DCreate9(void)
{
    uint32_t sdk = arg32(0);
    uint32_t obj = com_create_named("IDirect3D9", d3d9_methods,
                              (int)(sizeof(d3d9_methods) / sizeof(method_t)), 0);
    log_printf("d3d9: Direct3DCreate9(%u) -> %08x", sdk, obj);
    RET32(obj);
}

const import_def_t d3d9_imports[] = {
    {"d3d9.dll", "Direct3DCreate9", Direct3DCreate9, 1},
    {NULL, NULL, NULL, 0},
};
