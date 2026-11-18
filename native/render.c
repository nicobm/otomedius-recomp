/* render.c - the native renderer.
 *
 * Fixed function state and draw submission are collected here.  The actual
 * GPU work happens in the platform backend (SDL3 + Vulkan, platform.c); for
 * now the backend only clears the framebuffer and presents, which is enough
 * to run the game logic while the shader translation is being finished.
 */

#define _GNU_SOURCE
#include "win32.h"
#include "d3d9.h"
#include "platform.h"
#include "movie.h"
#include "xmodel.h"

#include <string.h>
#include <stdio.h>

static int g_width = 1280, g_height = 1024;
static uint32_t g_clear = 0xFF000000u;

/* software frame buffer, 0xAARRGGBB (BGRA byte order, what SDL wants) */
static uint8_t *g_fb;
static int g_fb_w, g_fb_h;
/* Viewport (D3DVIEWPORT9) and depth buffer for the XYZ vertex path. */
static float g_vp_x, g_vp_y, g_vp_w = 1280.0f, g_vp_h = 1024.0f;
static float *g_zbuf;
static int g_zb_w, g_zb_h;
static size_t g_zb_cap;
/* Framebuffer lives in emulated memory (see render_init).  g_fb is the
 * current render target: the back buffer, or a bound scene texture. */
static uint32_t g_fb_guest;
static size_t g_fb_bytes;
static uint8_t *g_fb_primary;
static void fb_fill(uint32_t color);

/* Depth buffer bound to the current render target.  Its stride has to follow
 * the target's width: draw_tri() indexes the z buffer with the target width,
 * so drawing into a smaller texture while keeping the back-buffer stride read
 * depth from the wrong rows. */
static void zbuf_bind(void)
{
    size_t n = (size_t)g_fb_w * (size_t)g_fb_h;
    if (n > g_zb_cap) {
        float *z = (float *)realloc(g_zbuf, n * sizeof(float));
        if (!z) {
            free(g_zbuf);
            g_zbuf = NULL;
            g_zb_cap = 0;
            n = 0;
        } else {
            g_zbuf = z;
            g_zb_cap = n;
        }
    }
    g_zb_w = g_fb_w;
    g_zb_h = g_fb_h;
    if (g_zbuf)
        for (size_t i = 0; i < n; i++)
            g_zbuf[i] = 1.0f;
}

/* ------------------------------------------------- renderer bring-up stats
 * Everything the game submits is counted per frame so a single log line says
 * whether the picture is missing because the game draws nothing or because the
 * platform backend throws it away. */
static struct {
    unsigned presents, clears, draws, draw_ups, verts;
    int minx, miny, maxx, maxy;   /* bounding box of the submitted quads */
    unsigned onscreen;
    unsigned model_draws, model_faces;
    int model_minx, model_miny, model_maxx, model_maxy;
    unsigned model_clip, model_zout;   /* faces dropped / outside the z range */
    unsigned long px, px_model;        /* pixels written into the target */
} g_stat;
/* Set while render_draw_model() rasterises, so fb_write() can attribute the
 * pixels to the model instead of the 2D HUD. */
static int g_in_model;
static int g_cur_ntex;
static uint32_t g_fvf, g_stream_stride, g_stream_vb, g_tex0;
static uint32_t g_first_draw_logged;
/* OTOMEDIUS_ORDERDBG=1: submission order (clears, 2D quads, model draws).
 * OTOMEDIUS_ORDERDBG=<present>: only from that present on, which keeps the
 * log small when the interesting window is late in the attract. */
static int g_orderdbg;
static unsigned g_order_from;
static unsigned g_order_seq;
static unsigned g_last_model_draws;

static int order_on(void)
{
    return g_orderdbg && g_stat.presents >= g_order_from;
}

int render_width(void) { return g_width; }
int render_height(void) { return g_height; }

static void render_state_defaults(void);

void render_init(int w, int h)
{
    const char *odb = getenv("OTOMEDIUS_ORDERDBG");
    g_orderdbg = odb != NULL;
    g_order_from = odb ? (unsigned)atoi(odb) : 0u;
    render_state_defaults();
    if (w > 0 && h > 0) {
        g_width = (int)w;
        g_height = (int)h;
    }
    /* The back buffer lives in emulated memory so the game can read it back
     * (GetRenderTargetData / LockRect on the back buffer surface).  It is
     * reused across resets instead of leaking a new 5 MiB bump allocation. */
    {
        extern uint32_t emu_alloc(uint32_t size);
        size_t bytes = (size_t)g_width * (size_t)g_height * 4u;
        if (g_fb_primary && g_fb_guest && g_fb_bytes == bytes) {
            memset(g_fb_primary, 0, bytes);
        } else {
            g_fb_guest = emu_alloc((uint32_t)bytes);
            g_fb_bytes = bytes;
            g_fb_primary = MEM + g_fb_guest;
            memset(g_fb_primary, 0, bytes);
        }
    }
    g_fb = g_fb_primary;
    g_fb_w = g_width;
    g_fb_h = g_height;
    zbuf_bind();
    g_vp_x = g_vp_y = 0.0f;
    g_vp_w = (float)g_fb_w;
    g_vp_h = (float)g_fb_h;
    fb_fill(g_clear);
    g_stat.minx = g_stat.miny = 0x7FFFFFFF;
    g_stat.maxx = g_stat.maxy = -0x7FFFFFFF;
    platform_set_size(g_width, g_height);
}

/* --------------------------------------------------------------- drawing
 * Everything the game submits reaches us as screen-space vertices
 * (D3DFVF_XYZRHW), so a small software rasteriser is enough to show the game
 * until the shader path is translated.  It handles the cases actually seen:
 *
 *   FVF 0x144 = XYZRHW | DIFFUSE | TEX1, stride 28, TRIANGLESTRIP / LIST
 *   D3DPT_*   4 = triangle list, 5 = strip, 6 = fan
 *
 * Pixels are 0xAARRGGBB in a BGRA byte buffer, which is what SDL wants. */
const uint8_t *render_framebuffer(int *w, int *h)
{
    if (w)
        *w = g_width;
    if (h)
        *h = g_height;
    return g_fb_primary;
}

/* Guest (emulated) address of the framebuffer, for the back buffer surface. */
uint32_t render_framebuffer_guest(void) { return g_fb_guest; }

/* Bind the destination of Clear/Draw* to a texture surface (drop the null
 * pointer to go back to the back buffer).  The game renders its 3D scene into
 * a scene texture this way and then composites it under the 2D HUD. */
void render_set_target(uint32_t guest, int w, int h)
{
    int bb = (!guest || w <= 0 || h <= 0);
    if (bb) {
        g_fb = g_fb_primary;
        g_fb_w = g_width;
        g_fb_h = g_height;
        zbuf_bind();
    } else {
        g_fb = MEM + guest;
        g_fb_w = w;
        g_fb_h = h;
        zbuf_bind();
    }
    if (getenv("OTOMEDIUS_RTDBG"))
        fprintf(stderr, "render: target %s %dx%d guest=%08x present=%u\n",
                bb ? "backbuffer" : "texture", g_fb_w, g_fb_h,
                bb ? 0u : guest, g_stat.presents);
}

void render_dump_fb(const char *path)
{
    FILE *f;
    /* Always dump the composed back buffer, never a bound render target. */
    const uint8_t *src = g_fb_primary ? g_fb_primary : g_fb;
    if (!path || !*path || !src || g_width <= 0 || g_height <= 0)
        return;
    f = fopen(path, "wb");
    if (!f) {
        log_printf("render: cannot write dump %s", path);
        return;
    }
    log_printf("render: dumped frame %u (%dx%d) to %s", g_stat.presents,
               g_width, g_height, path);
    fprintf(f, "P6\n%d %d\n255\n", g_width, g_height);
    for (size_t i = 0, n = (size_t)g_width * (size_t)g_height; i < n; i++) {
        uint32_t c = ((const uint32_t *)src)[i];
        fputc((c >> 16) & 0xFF, f);
        fputc((c >> 8) & 0xFF, f);
        fputc(c & 0xFF, f);
    }
    fclose(f);
}

static void fb_fill(uint32_t color)
{
    if (!g_fb)
        return;
    for (size_t i = 0, n = (size_t)g_fb_w * g_fb_h; i < n; i++)
        ((uint32_t *)g_fb)[i] = color | 0xFF000000u;
}

static inline void fb_blend(int x, int y, uint32_t src)
{
    uint32_t a = src >> 24;
    uint32_t *d;
    if (a == 0 || x < 0 || y < 0 || x >= g_fb_w || y >= g_fb_h)
        return;
    d = &((uint32_t *)g_fb)[(size_t)y * g_fb_w + x];
    if (a == 255) {
        *d = src;
        return;
    }
    {
        uint32_t s = src, dd = *d;
        uint32_t ia = 255 - a;
        uint32_t r = (((s >> 16) & 0xFF) * a + ((dd >> 16) & 0xFF) * ia) / 255;
        uint32_t g = (((s >> 8) & 0xFF) * a + ((dd >> 8) & 0xFF) * ia) / 255;
        uint32_t b = ((s & 0xFF) * a + (dd & 0xFF) * ia) / 255;
        *d = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}

typedef struct {
    float x, y, u, v;
    float z;        /* NDC depth 0..1 for the 3D path */
    float iw;       /* 1/w, for perspective-correct UV interpolation */
    uint32_t color;
    int valid;      /* 0 when the vertex was clipped (w <= 0) */
} svtx_t;

/* ---- fixed-function state the software rasteriser honours ---------------
 * Render states (D3DRS_*), stage-0 texture stage states (D3DTSS_*) and
 * stage-0 sampler states (D3DSAMP_*).  Defaults follow the D3D9 documentation. */
static uint32_t g_rs[256];
static uint32_t g_tss[32];
static uint32_t g_ss[16];
/* WORLD / VIEW / PROJECTION as last set by SetTransform (D3D row-major). */
static float g_xform[3][16];
/* Last matrix bound through ID3DXEffect::SetMatrix (the effect path uses it
 * instead of SetTransform; kept for the future 3D vertex path). */
static float g_effect_xform[16];
/* Index buffer bound by SetIndices and the vertex stream offset. */
static uint32_t g_ib, g_ib_base, g_stream_off;
enum { RS_ZENABLE = 7, RS_ZWRITEENABLE = 14, RS_ALPHATESTENABLE = 15,
       RS_SRCBLEND = 19, RS_DESTBLEND = 20, RS_ZFUNC = 23, RS_ALPHAREF = 24,
       RS_ALPHAFUNC = 25, RS_ALPHABLENDENABLE = 27, RS_TEXTUREFACTOR = 60 };

static void render_state_defaults(void)
{
    static const float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    memset(g_rs, 0, sizeof g_rs);
    /* D3D9 defaults: WORLD/VIEW/PROJECTION start as identity and GetTransform
     * returns them.  The game copies the device VIEW into the shader's "View"
     * constant, so a zeroed matrix here made every 3D vertex collapse. */
    memcpy(g_xform[0], ident, sizeof ident);
    memcpy(g_xform[1], ident, sizeof ident);
    memcpy(g_xform[2], ident, sizeof ident);
    memset(g_effect_xform, 0, sizeof g_effect_xform);
    g_rs[RS_SRCBLEND] = 2;           /* ONE */
    g_rs[RS_DESTBLEND] = 1;          /* ZERO */
    g_rs[RS_ALPHAFUNC] = 8;          /* ALWAYS */
    g_rs[RS_TEXTUREFACTOR] = 0xFFFFFFFFu;
    g_rs[RS_ZENABLE] = 1;            /* TRUE */
    g_rs[RS_ZWRITEENABLE] = 1;       /* TRUE */
    g_rs[RS_ZFUNC] = 4;              /* D3DCMP_LESSEQUAL */
    memset(g_tss, 0, sizeof g_tss);
    g_tss[1] = 4;                    /* COLOROP   = MODULATE */
    g_tss[2] = 2;                    /* COLORARG1 = TEXTURE */
    g_tss[3] = 1;                    /* COLORARG2 = CURRENT */
    g_tss[4] = 2;                    /* ALPHAOP   = SELECTARG1 */
    g_tss[5] = 2;                    /* ALPHAARG1 = TEXTURE */
    g_tss[6] = 1;                    /* ALPHAARG2 = CURRENT */
    memset(g_ss, 0, sizeof g_ss);
    g_ss[1] = g_ss[2] = 1;           /* ADDRESSU/V = WRAP */
    g_ss[5] = g_ss[6] = 1;           /* MAG/MINFILTER = POINT */
}

void render_set_texture_stage_state(uint32_t stage, uint32_t type, uint32_t value)
{
    if (stage == 0 && type < 32)
        g_tss[type] = value;
}

static inline uint32_t clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint32_t)v; }

static uint32_t tex_texel(const uint32_t *tex, int tw, int th, int tx, int ty,
                          uint32_t modeu, uint32_t modev)
{
    if ((unsigned)tx >= (unsigned)tw) {
        if (modeu == 3) { tx = tx < 0 ? 0 : tw - 1; }
        else if (modeu == 2) { tx %= 2 * tw; if (tx < 0) tx += 2 * tw; if (tx >= tw) tx = 2 * tw - 1 - tx; }
        else { tx %= tw; if (tx < 0) tx += tw; }
    }
    if ((unsigned)ty >= (unsigned)th) {
        if (modev == 3) { ty = ty < 0 ? 0 : th - 1; }
        else if (modev == 2) { ty %= 2 * th; if (ty < 0) ty += 2 * th; if (ty >= th) ty = 2 * th - 1 - ty; }
        else { ty %= th; if (ty < 0) ty += th; }
    }
    return tex[(size_t)ty * (size_t)tw + (size_t)tx];
}

static uint32_t tex_sample(const uint32_t *tex, uint32_t tw, uint32_t th,
                           float u, float v)
{
    uint32_t mu = g_ss[1], mv = g_ss[2];
    if (g_ss[5] == 2 || g_ss[6] == 2) {          /* LINEAR */
        float fx = u * (float)tw - 0.5f, fy = v * (float)th - 0.5f;
        int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
        float ax = fx - (float)x0, ay = fy - (float)y0;
        uint32_t c00 = tex_texel(tex, (int)tw, (int)th, x0, y0, mu, mv);
        uint32_t c10 = tex_texel(tex, (int)tw, (int)th, x0 + 1, y0, mu, mv);
        uint32_t c01 = tex_texel(tex, (int)tw, (int)th, x0, y0 + 1, mu, mv);
        uint32_t c11 = tex_texel(tex, (int)tw, (int)th, x0 + 1, y0 + 1, mu, mv);
        uint32_t out = 0;
        for (int sh = 0; sh < 32; sh += 8) {
            float t0 = (float)((c00 >> sh) & 255) * (1 - ax) + (float)((c10 >> sh) & 255) * ax;
            float t1 = (float)((c01 >> sh) & 255) * (1 - ax) + (float)((c11 >> sh) & 255) * ax;
            out |= clamp255((int)(t0 * (1 - ay) + t1 * ay + 0.5f)) << sh;
        }
        return out;
    }
    return tex_texel(tex, (int)tw, (int)th, (int)floorf(u * (float)tw),
                     (int)floorf(v * (float)th), mu, mv);
}

/* D3DTA_*: DIFFUSE 0, CURRENT 1, TEXTURE 2, TFACTOR 3; flags 0x10 complement,
 * 0x20 alpha replicate */
static uint32_t tss_arg(uint32_t a, uint32_t diffuse, uint32_t current,
                        uint32_t texel)
{
    uint32_t c;
    switch (a & 15) {
    case 0: c = diffuse; break;
    case 1: c = current; break;
    case 2: c = texel; break;
    case 3: c = g_rs[RS_TEXTUREFACTOR]; break;
    default: c = 0xFFFFFFFFu; break;
    }
    if (a & 0x20) c = (c & 0xFF000000u) | ((c >> 24) * 0x010101u);
    if (a & 0x10) c ^= 0x00FFFFFFu;
    return c;
}

static uint32_t tss_op(uint32_t op, uint32_t a1, uint32_t a2, int shift_from,
                       int is_alpha)
{
    uint32_t out = 0;
    int lo = is_alpha ? 24 : 0, hi = is_alpha ? 32 : 24;
    (void)shift_from;
    for (int sh = lo; sh < hi; sh += 8) {
        int x = (int)((a1 >> sh) & 255), y = (int)((a2 >> sh) & 255), r;
        switch (op) {
        case 2: r = x; break;
        case 3: r = y; break;
        case 4: r = x * y / 255; break;
        case 5: r = x * y * 2 / 255; break;
        case 6: r = x * y * 4 / 255; break;
        case 7: r = x + y; break;
        case 8: r = x + y - 128; break;
        case 10: r = x - y; break;
        default: r = x; break;
        }
        out |= clamp255(r) << sh;
    }
    return out;
}

static uint32_t blend_factor(uint32_t f, uint32_t src, uint32_t dst, int sh)
{
    uint32_t sa = src >> 24, da = dst >> 24, c;
    switch (f) {
    case 1: return 0;
    case 2: return 255;
    case 3: c = (src >> sh) & 255; return c;
    case 4: c = (src >> sh) & 255; return 255 - c;
    case 5: return sa;
    case 6: return 255 - sa;
    case 7: return da;
    case 8: return 255 - da;
    case 9: return (dst >> sh) & 255;
    case 10: return 255 - ((dst >> sh) & 255);
    default: return 255;
    }
}

static void fb_write(int x, int y, uint32_t src)
{
    uint32_t *d;
    if (x < 0 || y < 0 || x >= g_fb_w || y >= g_fb_h)
        return;
    if (g_rs[RS_ALPHATESTENABLE]) {
        uint32_t a = src >> 24, ref = g_rs[RS_ALPHAREF] & 255;
        int pass;
        switch (g_rs[RS_ALPHAFUNC]) {
        case 1: pass = 0; break;
        case 2: pass = a < ref; break;
        case 3: pass = a == ref; break;
        case 4: pass = a <= ref; break;
        case 5: pass = a > ref; break;
        case 6: pass = a != ref; break;
        case 7: pass = a >= ref; break;
        default: pass = 1; break;
        }
        if (!pass)
            return;
    }
    d = &((uint32_t *)g_fb)[(size_t)y * g_fb_w + x];
    g_stat.px++;
    if (g_in_model)
        g_stat.px_model++;
    if (!g_rs[RS_ALPHABLENDENABLE]) {
        *d = src | 0xFF000000u;
        return;
    }
    {
        uint32_t dd = *d, out = 0xFF000000u;
        for (int sh = 0; sh < 24; sh += 8) {
            uint32_t sf = blend_factor(g_rs[RS_SRCBLEND], src, dd, sh);
            uint32_t df = blend_factor(g_rs[RS_DESTBLEND], src, dd, sh);
            uint32_t v = ((((src >> sh) & 255) * sf) + (((dd >> sh) & 255) * df)) / 255;
            out |= clamp255((int)v) << sh;
        }
        *d = out;
    }
}


static void draw_tri(const svtx_t *a, const svtx_t *b, const svtx_t *c,
                     const uint32_t *tex, uint32_t tw, uint32_t th, int depth)
{
    float minx = a->x, maxx = a->x, miny = a->y, maxy = a->y;
    float area;
    int x0, y0, x1, y1;

    if (!a->valid || !b->valid || !c->valid)
        return;
    if (b->x < minx) minx = b->x;
    if (c->x < minx) minx = c->x;
    if (b->x > maxx) maxx = b->x;
    if (c->x > maxx) maxx = c->x;
    if (b->y < miny) miny = b->y;
    if (c->y < miny) miny = c->y;
    if (b->y > maxy) maxy = b->y;
    if (c->y > maxy) maxy = c->y;

    area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (area > -0.0001f && area < 0.0001f)
        return;                       /* degenerate */
    /* Keep the SIGN.  The game submits clockwise quads (negative area); using
     * fabs() made the barycentric weights sum to -1, so every interpolated
     * UV/colour came out negated.  With WRAP addressing that flipped textured
     * quads by 180 degrees (the eAMUSE logo) and sampled the wrong atlas cell
     * for text. */

    x0 = (int)minx; y0 = (int)miny;
    x1 = (int)(maxx + 0.999f); y1 = (int)(maxy + 0.999f);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_fb_w) x1 = g_fb_w;
    if (y1 > g_fb_h) y1 = g_fb_h;

    int flat = a->color == b->color && b->color == c->color;
    uint32_t flat_diff = a->color;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = ((b->x - px) * (c->y - py) - (b->y - py) * (c->x - px));
            float w1 = ((c->x - px) * (a->y - py) - (c->y - py) * (a->x - px));
            float w2 = ((a->x - px) * (b->y - py) - (a->y - py) * (b->x - px));
            float l0, l1, l2, u, v;
            float iw, z = 0.0f;
            uint32_t col;

            /* Barycentric inside test.  w0/w1/w2 all have the same sign as
             * the triangle area for a point inside it; a mixed-sign set is
             * outside.  (The old test skipped the inside and drew the
             * complement, which made single triangles disappear and made
             * every quad's two halves sample the other triangle's UVs.) */
            if (!((w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) ||
                  (w0 <= 0.0f && w1 <= 0.0f && w2 <= 0.0f)))
                continue;
            l0 = w0 / area; l1 = w1 / area; l2 = w2 / area;
            /* Perspective-correct texture coordinates: interpolate u/w and
             * v/w linearly in screen space, then divide by the interpolated
             * 1/w.  The 2D path stores iw = 1, so this is a no-op there. */
            iw = a->iw * l0 + b->iw * l1 + c->iw * l2;
            if (iw > 1e-12f || iw < -1e-12f) {
                u = (a->u * a->iw * l0 + b->u * b->iw * l1 +
                     c->u * c->iw * l2) / iw;
                v = (a->v * a->iw * l0 + b->v * b->iw * l1 +
                     c->v * c->iw * l2) / iw;
            } else {
                u = a->u * l0 + b->u * l1 + c->u * l2;
                v = a->v * l0 + b->v * l1 + c->v * l2;
            }
            if (depth) {
                size_t zi = (size_t)y * (size_t)g_fb_w + (size_t)x;
                float zf;
                z = a->z * l0 + b->z * l1 + c->z * l2;
                if (g_rs[RS_ZENABLE] && g_zbuf) {
                    zf = g_zbuf[zi];
                    /* D3DCMP_* values: 1 NEVER, 2 LESS, 3 EQUAL, 4 LESSEQUAL,
                     * 5 GREATER, 6 NOTEQUAL, 7 GREATEREQUAL, 8 ALWAYS.  The
                     * old table was shifted by one, so the default LESSEQUAL
                     * behaved as GREATEREQUAL: every 3D vertex in front of the
                     * far plane was culled and the composite quad (z = 0) too. */
                    switch (g_rs[RS_ZFUNC]) {
                    case 1: continue;                             /* NEVER */
                    case 2: if (!(z < zf)) continue; break;       /* LESS */
                    case 3: if (!(z == zf)) continue; break;      /* EQUAL */
                    case 4: if (!(z <= zf)) continue; break;      /* LESSEQUAL */
                    case 5: if (!(z > zf)) continue; break;       /* GREATER */
                    case 6: if (!(z != zf)) continue; break;      /* NOTEQUAL */
                    case 7: if (!(z >= zf)) continue; break;      /* GREATEREQUAL */
                    default: break;                               /* ALWAYS */
                    }
                }
                if (g_rs[RS_ZWRITEENABLE] && g_zbuf)
                    g_zbuf[zi] = z;
            }

            {
                uint32_t texel = 0xFFFFFFFFu, diff, cur, res, ares;
                uint32_t dA, dR, dG, dB;
                if (tex && tw && th)
                    texel = tex_sample(tex, tw, th, u, v);
                if (flat) {
                    diff = flat_diff;
                } else {
                dA = clamp255((int)(((a->color >> 24) & 255) * l0 + ((b->color >> 24) & 255) * l1 + ((c->color >> 24) & 255) * l2 + 0.5f));
                dR = clamp255((int)(((a->color >> 16) & 255) * l0 + ((b->color >> 16) & 255) * l1 + ((c->color >> 16) & 255) * l2 + 0.5f));
                dG = clamp255((int)(((a->color >> 8) & 255) * l0 + ((b->color >> 8) & 255) * l1 + ((c->color >> 8) & 255) * l2 + 0.5f));
                dB = clamp255((int)((a->color & 255) * l0 + (b->color & 255) * l1 + (c->color & 255) * l2 + 0.5f));
                diff = (dA << 24) | (dR << 16) | (dG << 8) | dB;
                }
                cur = diff;
                col = texel;
                if (!(tex && tw && th)) {
                    res = cur;
                    ares = cur;
                } else if (g_tss[1] == 1) {
                    res = cur;
                } else {
                    res = tss_op(g_tss[1], tss_arg(g_tss[2], diff, cur, texel),
                                 tss_arg(g_tss[3], diff, cur, texel), 0, 0);
                }
                if (!(tex && tw && th)) {
                    /* untextured: diffuse alpha already in ares */
                } else if (g_tss[4] == 1) {
                    ares = cur;
                } else {
                    ares = tss_op(g_tss[4], tss_arg(g_tss[5], diff, cur, texel),
                                  tss_arg(g_tss[6], diff, cur, texel), 0, 1);
                }
                fb_write(x, y, (ares & 0xFF000000u) | (res & 0x00FFFFFFu));
            }
        }
    }
}

/* Rasterise an immediate vertex array (DrawPrimitiveUP). */
static void drawdbg_skip(const char *why, uint32_t prim, uint32_t count,
                         uint32_t data, uint32_t stride, uint32_t nverts)
{
    static int on = -1;
    static unsigned n;
    if (on < 0)
        on = getenv("OTOMEDIUS_DRAWDBG") != NULL;
    if (on && n < 40) {
        n++;
        fprintf(stderr, "drawdbg[%u]: SKIP(%s) prim=%u count=%u n=%u "
                "data=%08x stride=%u fvf=%06x\n", g_stat.presents, why, prim,
                count, nverts, data, stride, g_fvf);
    }
}

/* ---- FVF layout and the XYZ (non-RHW) vertex pipeline ------------------
 * The 3D path reads the vertex stream from the buffers bound with
 * SetStreamSource/SetIndices, transforms XYZ positions by WORLD*VIEW*PROJ
 * (D3D row-vector convention), divides by w, maps through the viewport and
 * interpolates UVs perspective-correctly.  The 2D path (XYZRHW) keeps the
 * original screen-space behaviour. */
typedef struct {
    uint32_t fvf;
    int pos3d;
    uint32_t pos_size, diff_off, tex_off;
    int has_diff, ntex;
    uint32_t min_stride;
} fvfinfo_t;

static void fvf_parse(uint32_t fvf, fvfinfo_t *fi)
{
    uint32_t pos = fvf & 0x00Eu, off;
    memset(fi, 0, sizeof *fi);
    fi->fvf = fvf;
    if (pos == 0x004u) {                     /* XYZRHW */
        off = 16u;
    } else {                                 /* XYZ / XYZB1..XYZB5 */
        uint32_t nblend = pos >= 0x006u ? (pos - 0x004u) / 2u : 0u;
        fi->pos3d = 1;
        off = 12u + nblend * 4u;
    }
    fi->pos_size = off;
    if (fvf & 0x010u) off += 12u;            /* NORMAL */
    if (fvf & 0x020u) off += 4u;             /* PSIZE */
    if (fvf & 0x040u) { fi->has_diff = 1; fi->diff_off = off; off += 4u; }
    if (fvf & 0x080u) off += 4u;             /* SPECULAR */
    fi->ntex = (int)((fvf >> 8) & 15u);
    fi->tex_off = off;
    off += (uint32_t)fi->ntex * 8u;          /* 2D texture coordinates */
    fi->min_stride = off;
}

static void mat_mul_row(float *out, const float *a, const float *b)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++)
                s += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = s;
        }
}

/* Build WORLD*VIEW*PROJ.  Returns 0 when no usable matrix is bound. */
static int xform_wvp(float *out)
{
    float wv[16];
    const char *e = getenv("OTOMEDIUS_USE_EFFECT_XFORM");
    int have_world = 0, have_view = 0, have_proj = 0;
    for (int i = 0; i < 16; i++) {
        if (g_xform[2][i] != 0.0f) have_world = 1;
        if (g_xform[0][i] != 0.0f) have_view = 1;
        if (g_xform[1][i] != 0.0f) have_proj = 1;
    }
    if (e && *e && *e != '0') {
        memcpy(out, g_effect_xform, sizeof g_effect_xform);
        return 1;
    }
    if (!have_proj) {
        /* Effect-only scenes may only provide a combined WorldViewProj. */
        memcpy(out, g_effect_xform, sizeof g_effect_xform);
        return have_world || have_view;
    }
    if (!have_world) {
        static const float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        mat_mul_row(wv, ident, g_xform[0]);
    } else {
        mat_mul_row(wv, g_xform[2], g_xform[0]);
    }
    mat_mul_row(out, wv, g_xform[1]);
    return 1;
}

static void make_svtx(const uint8_t *v, const fvfinfo_t *fi, const float *wvp,
                      int use_wvp, svtx_t *out)
{
    const float *f = (const float *)v;
    memset(out, 0, sizeof *out);
    out->color = fi->has_diff ? M32((uint32_t)(v - MEM) + fi->diff_off)
                              : 0xFFFFFFFFu;
    out->iw = 1.0f;
    out->valid = 1;
    if (fi->pos3d && use_wvp) {
        float x = f[0], y = f[1], z = f[2];
        float cx = x * wvp[0] + y * wvp[4] + z * wvp[8]  + wvp[12];
        float cy = x * wvp[1] + y * wvp[5] + z * wvp[9]  + wvp[13];
        float cz = x * wvp[2] + y * wvp[6] + z * wvp[10] + wvp[14];
        float cw = x * wvp[3] + y * wvp[7] + z * wvp[11] + wvp[15];
        if (cw <= 1e-6f) {                   /* behind the eye: clipped */
            out->valid = 0;
            return;
        }
        out->x = g_vp_x + (cx / cw + 1.0f) * 0.5f * g_vp_w;
        out->y = g_vp_y + (1.0f - cy / cw) * 0.5f * g_vp_h;
        out->z = cz / cw;
        out->iw = 1.0f / cw;
        if (fi->ntex > 0) {
            const float *t = (const float *)(v + fi->tex_off);
            out->u = t[0] * out->iw;
            out->v = t[1] * out->iw;
        }
        return;
    }
    /* Screen-space path (XYZRHW): the game already divided by w. */
    out->x = f[0];
    out->y = f[1];
    out->z = f[2];
    if (fi->ntex > 0) {
        const float *t = (const float *)(v + fi->tex_off);
        out->u = t[0];
        out->v = t[1];
    }
}

static void raster_array(svtx_t *vt, uint32_t nverts, uint32_t prim,
                         uint32_t *tex, uint32_t tw, uint32_t th, int depth)
{
    if (prim == 4) {
        for (uint32_t i = 0; i + 2 < nverts; i += 3)
            draw_tri(&vt[i], &vt[i + 1], &vt[i + 2], tex, tw, th, depth);
    } else if (prim == 5) {
        for (uint32_t i = 0; i + 2 < nverts; i++) {
            if (i & 1)
                draw_tri(&vt[i + 1], &vt[i], &vt[i + 2], tex, tw, th, depth);
            else
                draw_tri(&vt[i], &vt[i + 1], &vt[i + 2], tex, tw, th, depth);
        }
    } else if (prim == 6) {
        for (uint32_t i = 1; i + 1 < nverts; i++)
            draw_tri(&vt[0], &vt[i], &vt[i + 1], tex, tw, th, depth);
    }
}

/* Rasterise an immediate vertex array (DrawPrimitiveUP). */
static void draw_up_vertices(uint32_t prim, uint32_t prim_count, uint32_t data,
                             uint32_t stride)
{
    const uint8_t *base;
    uint32_t nverts, *tex = NULL, tw = 0, th = 0;
    svtx_t *vt;
    fvfinfo_t fi;
    float wvp[16];
    int use_wvp = 0;

    if (!g_fb || !data || !stride) {
        drawdbg_skip("no-data", prim, prim_count, data, stride, 0);
        return;
    }
    switch (prim) {
    case 2: nverts = prim_count * 2; break;            /* line list */
    case 3: nverts = prim_count + 1; break;            /* line strip */
    case 4: nverts = prim_count * 3; break;            /* triangle list */
    case 5: nverts = prim_count + 2; break;            /* triangle strip */
    case 6: nverts = prim_count + 2; break;            /* triangle fan */
    default: drawdbg_skip("prim", prim, prim_count, data, stride, 0); return;
    }
    if (nverts < 3 || nverts > 65536) {
        drawdbg_skip("nverts", prim, prim_count, data, stride, nverts);
        return;
    }
    fvf_parse(g_fvf ? g_fvf : 0x144u, &fi);
    if (stride < fi.min_stride) {
        drawdbg_skip("stride", prim, prim_count, data, stride, nverts);
        return;
    }
    if (fi.pos3d) {
        use_wvp = xform_wvp(wvp);
        if (!use_wvp) {
            drawdbg_skip("no-wvp", prim, prim_count, data, stride, nverts);
            return;
        }
    }
    vt = (svtx_t *)calloc(nverts, sizeof(*vt));
    if (!vt) {
        drawdbg_skip("calloc", prim, prim_count, data, stride, nverts);
        return;
    }
    base = MEM + data;
    for (uint32_t i = 0; i < nverts; i++)
        make_svtx(base + (size_t)i * stride, &fi, wvp, use_wvp, &vt[i]);
    g_cur_ntex = fi.ntex;

    if (getenv("OTOMEDIUS_VERTDBG")) {
        static unsigned frame_no;
        if ((frame_no++ % 120) == 0) {
            fprintf(stderr, "render: verts prim=%u n=%u stride=%u fvf=%x:",
                    prim, nverts, stride, fi.fvf);
            for (uint32_t i = 0; i < nverts && i < 4; i++)
                fprintf(stderr, " (%.1f,%.1f z=%.3f uv=%.2f,%.2f)", vt[i].x,
                        vt[i].y, vt[i].z, vt[i].u, vt[i].v);
            fprintf(stderr, "  fb=%dx%d\n", g_fb_w, g_fb_h);
        }
    }

    /* The game's font renderer passes texture coordinates in *cell* units:
     * a glyph at column 6, row 4 arrives as u=6.0, v=4.0, and the quad's
     * second corner is u=6.09375 (one cell).  Treating those as normalised
     * UVs makes WRAP sample texel 0 for every glyph.  Detect that convention
     * (out-of-range integer UVs with a sub-cell span) and expand the integer
     * part with the cell size taken from this quad.  Only for 2D draws: the
     * 3D path uses real normalised coordinates. */
    if (!fi.pos3d) {
        float minu = vt[0].u, maxu = vt[0].u, minv = vt[0].v, maxv = vt[0].v;
        float cell_u, cell_v;
        int cell_uv;
        for (uint32_t i = 1; i < nverts; i++) {
            if (vt[i].u < minu) minu = vt[i].u;
            if (vt[i].u > maxu) maxu = vt[i].u;
            if (vt[i].v < minv) minv = vt[i].v;
            if (vt[i].v > maxv) maxv = vt[i].v;
        }
        cell_u = maxu - minu;
        cell_v = maxv - minv;
        cell_uv = (maxu >= 1.5f || maxv >= 1.5f) &&
                  cell_u > 0.0f && cell_v > 0.0f &&
                  cell_u < 1.0f && cell_v < 1.0f;
        if (cell_uv) {
            for (uint32_t i = 0; i < nverts; i++) {
                vt[i].u = floorf(vt[i].u) * cell_u +
                          (vt[i].u - floorf(vt[i].u));
                vt[i].v = floorf(vt[i].v) * cell_v +
                          (vt[i].v - floorf(vt[i].v));
            }
        }
    }

    tw = th = 0;
    if (g_tex0 && g_cur_ntex > 0) {
        uint32_t w = 0, h = 0;
        const uint32_t *px = texture_argb(g_tex0, &w, &h);
        if (px) {
            tex = (uint32_t *)px;
            tw = w;
            th = h;
            /* OTOMEDIUS_TEXDUMP=WxH dumps the first matching decoded texture
             * as PPM, which is how a bad DXT decode is told apart from a bad
             * palette or UV mapping. */
            {
                const char *e = getenv("OTOMEDIUS_TEXDUMP");
                static int done;
                if (e && !done) {
                    unsigned fw = 0, fh = 0;
                    if (sscanf(e, "%ux%u", &fw, &fh) == 2 &&
                        fw == w && fh == h) {
                        char path[256];
                        done = 1;
                        snprintf(path, sizeof(path), "/tmp/opencode/tex_%08x.ppm",
                                 g_tex0);
                        FILE *fp = fopen(path, "wb");
                        if (fp) {
                            fprintf(fp, "P6\n%u %u\n255\n", w, h);
                            for (uint32_t i = 0; i < w * h; i++) {
                                uint32_t p = px[i];
                                fputc((int)((p >> 16) & 255), fp);
                                fputc((int)((p >> 8) & 255), fp);
                                fputc((int)(p & 255), fp);
                            }
                            fclose(fp);
                            fprintf(stderr, "render: dumped texture %08x %ux%u "
                                    "to %s\n", g_tex0, w, h, path);
                        }
                    }
                }
            }
        }
    }

    {
        static int dbg_at = -2;
        if (dbg_at == -2) {
            const char *e = getenv("OTOMEDIUS_DRAWDBG");
            dbg_at = e ? atoi(e) : -1;
        }
        if (dbg_at >= 0 && g_stat.presents == (unsigned)dbg_at) {
            fprintf(stderr, "drawdbg[%u]: prim=%u n=%u fvf=%x stride=%u tex=%08x %ux%u "
                    "caller=%08x "
                    "rs(ab=%u src=%u dst=%u at=%u z=%u/%u/%u) "
                    "tss(cop=%u c1=%u c2=%u aop=%u) "
                    "ss(u=%u v=%u mag=%u) v0=(%.0f,%.0f z%.3f uv %.3f,%.3f c=%08x) "
                    "v2=(%.0f,%.0f uv %.3f,%.3f)\n",
                    g_stat.presents, prim, nverts, g_fvf, stride, g_tex0, tw, th,
                    M32(x86_cpu()->esp),
                    g_rs[RS_ALPHABLENDENABLE], g_rs[RS_SRCBLEND], g_rs[RS_DESTBLEND],
                    g_rs[RS_ALPHATESTENABLE], g_rs[RS_ZENABLE],
                    g_rs[RS_ZWRITEENABLE], g_rs[RS_ZFUNC],
                    g_tss[1], g_tss[2], g_tss[3], g_tss[4],
                    g_ss[1], g_ss[2], g_ss[5], vt[0].x, vt[0].y, vt[0].z,
                    vt[0].u, vt[0].v, vt[0].color,
                    vt[nverts > 2 ? 2 : 0].x, vt[nverts > 2 ? 2 : 0].y,
                    vt[nverts > 2 ? 2 : 0].u, vt[nverts > 2 ? 2 : 0].v);
        }
    }

    /* frame statistics: is anything actually on screen? */
    {
        float minx = vt[0].x, maxx = vt[0].x, miny = vt[0].y, maxy = vt[0].y;
        for (uint32_t i = 1; i < nverts; i++) {
            if (vt[i].x < minx) minx = vt[i].x;
            if (vt[i].x > maxx) maxx = vt[i].x;
            if (vt[i].y < miny) miny = vt[i].y;
            if (vt[i].y > maxy) maxy = vt[i].y;
        }
        if ((int)minx < g_stat.minx) g_stat.minx = (int)minx;
        if ((int)miny < g_stat.miny) g_stat.miny = (int)miny;
        if ((int)maxx > g_stat.maxx) g_stat.maxx = (int)maxx;
        if ((int)maxy > g_stat.maxy) g_stat.maxy = (int)maxy;
        if (maxx > 0 && maxy > 0 && minx < g_fb_w && miny < g_fb_h)
            g_stat.onscreen++;
    }

    raster_array(vt, nverts, prim, tex, tw, th, fi.pos3d);
    free(vt);
}

/* Vertex/index-buffer path used by DrawPrimitive / DrawIndexedPrimitive. */
static void draw_vb(uint32_t prim, uint32_t base_vertex, uint32_t min_index,
                    uint32_t num_vertices, uint32_t start_index, int indexed,
                    uint32_t prim_count)
{
    const uint8_t *vbd, *ibd = NULL;
    uint32_t vbsize = 0, ibsize = 0, ibfmt = 0, nverts, *tex = NULL;
    uint32_t tw = 0, th = 0, stride = g_stream_stride, isz = 2;
    svtx_t *vt;
    fvfinfo_t fi;
    float wvp[16];
    int use_wvp = 0;

    (void)min_index;
    (void)num_vertices;
    if (!g_fb || !g_stream_vb || !stride)
        return;
    vbd = (const uint8_t *)buffer_data(g_stream_vb, &vbsize, NULL, NULL);
    if (!vbd)
        return;
    switch (prim) {
    case 2: nverts = prim_count * 2; break;
    case 3: nverts = prim_count + 1; break;
    case 4: nverts = prim_count * 3; break;
    case 5: nverts = prim_count + 2; break;
    case 6: nverts = prim_count + 2; break;
    default: return;
    }
    if (nverts < 3 || nverts > 262144)
        return;
    if (indexed) {
        int is_index = 0;
        ibd = (const uint8_t *)buffer_data(g_ib, &ibsize, &is_index, &ibfmt);
        if (!ibd || !is_index)
            return;
        isz = (ibfmt == 102u) ? 4u : 2u;      /* D3DFMT_INDEX32 */
        if ((size_t)(start_index + nverts) * isz > ibsize)
            return;
    }
    fvf_parse(g_fvf, &fi);
    if (stride < fi.min_stride)
        return;
    if (fi.pos3d) {
        use_wvp = xform_wvp(wvp);
        if (!use_wvp)
            return;
    }
    vt = (svtx_t *)calloc(nverts, sizeof(*vt));
    if (!vt)
        return;
    for (uint32_t i = 0; i < nverts; i++) {
        uint32_t vidx;
        if (indexed) {
            uint32_t raw = (isz == 4u)
                ? M32((uint32_t)(ibd - MEM) + (start_index + i) * 4u)
                : (uint32_t)M16((uint32_t)(ibd - MEM) + (start_index + i) * 2u);
            vidx = raw + base_vertex;
        } else {
            vidx = base_vertex + i;
        }
        if ((size_t)g_stream_off + (size_t)vidx * stride + fi.min_stride > vbsize) {
            vt[i].valid = 0;
            continue;
        }
        make_svtx(vbd + g_stream_off + (size_t)vidx * stride, &fi, wvp,
                  use_wvp, &vt[i]);
    }
    g_cur_ntex = fi.ntex;
    if (g_tex0 && fi.ntex > 0) {
        uint32_t w = 0, h = 0;
        const uint32_t *px = texture_argb(g_tex0, &w, &h);
        if (px) { tex = (uint32_t *)px; tw = w; th = h; }
    }
    if (getenv("OTOMEDIUS_DRAWDBG")) {
        static unsigned n;
        if (n++ < 20)
            fprintf(stderr, "drawdbg-vb[%u]: prim=%u n=%u fvf=%x stride=%u "
                    "vb=%08x off=%u ib=%08x start=%u base=%u tex=%08x %ux%u\n",
                    g_stat.presents, prim, nverts, g_fvf, stride, g_stream_vb,
                    g_stream_off, g_ib, start_index, base_vertex, g_tex0, tw, th);
    }
    raster_array(vt, nverts, prim, tex, tw, th, fi.pos3d);
    free(vt);
}

/* ---------------------------------------------------------------- HLE ship
 * The .x loader is not implemented yet.  D3DXLoadMeshHierarchyFromXInMemory
 * builds a frame hierarchy through the game's own ID3DXAllocateHierarchy and
 * attaches a container whose draw method lands here.  The geometry is a
 * synthetic ship so the whole chain (frame hierarchy -> game CModel ->
 * container draw -> XYZ rasteriser) can be exercised while the real .x mesh
 * parser is still pending.  OTOMEDIUS_NO_SHIP=1 disables it. */
typedef struct { float x, y, z; uint32_t color; } shipv_t;
static const shipv_t g_ship[] = {
    /* nose -> mid ring */
    {0.0f,0.0f,2.2f,0xFFC8D8F0u}, {0.45f,0.35f,0.3f,0xFFC8D8F0u}, {0.45f,-0.35f,0.3f,0xFFC8D8F0u},
    {0.0f,0.0f,2.2f,0xFFC8D8F0u}, {0.45f,-0.35f,0.3f,0xFFC8D8F0u}, {-0.45f,-0.35f,0.3f,0xFFC8D8F0u},
    {0.0f,0.0f,2.2f,0xFFC8D8F0u}, {-0.45f,-0.35f,0.3f,0xFFC8D8F0u}, {-0.45f,0.35f,0.3f,0xFFC8D8F0u},
    {0.0f,0.0f,2.2f,0xFFC8D8F0u}, {-0.45f,0.35f,0.3f,0xFFC8D8F0u}, {0.45f,0.35f,0.3f,0xFFC8D8F0u},
    /* mid ring -> tail */
    {0.45f,0.35f,0.3f,0xFFA0B0D0u}, {0.0f,0.0f,-2.0f,0xFFA0B0D0u}, {0.45f,-0.35f,0.3f,0xFFA0B0D0u},
    {0.45f,-0.35f,0.3f,0xFFA0B0D0u}, {0.0f,0.0f,-2.0f,0xFFA0B0D0u}, {-0.45f,-0.35f,0.3f,0xFFA0B0D0u},
    {-0.45f,-0.35f,0.3f,0xFFA0B0D0u}, {0.0f,0.0f,-2.0f,0xFFA0B0D0u}, {-0.45f,0.35f,0.3f,0xFFA0B0D0u},
    {-0.45f,0.35f,0.3f,0xFFA0B0D0u}, {0.0f,0.0f,-2.0f,0xFFA0B0D0u}, {0.45f,0.35f,0.3f,0xFFA0B0D0u},
    /* wings */
    {0.4f,0.0f,0.7f,0xFF4060C0u}, {2.9f,0.0f,-0.7f,0xFF4060C0u}, {0.4f,0.0f,-1.4f,0xFF3050A0u},
    {-0.4f,0.0f,0.7f,0xFF4060C0u}, {-0.4f,0.0f,-1.4f,0xFF3050A0u}, {-2.9f,0.0f,-0.7f,0xFF4060C0u},
    /* tail fin */
    {0.0f,0.0f,-1.4f,0xFFE8E8F0u}, {0.0f,1.1f,-2.3f,0xFFE8E8F0u}, {0.0f,0.0f,-2.3f,0xFFC0C0D0u},
    /* horizontal stabilisers */
    {0.0f,0.0f,-1.5f,0xFF8088A0u}, {1.3f,0.0f,-2.3f,0xFF8088A0u}, {0.0f,0.0f,-2.3f,0xFF606880u},
    {0.0f,0.0f,-1.5f,0xFF8088A0u}, {0.0f,0.0f,-2.3f,0xFF606880u}, {-1.3f,0.0f,-2.3f,0xFF8088A0u},
    /* cockpit */
    {0.0f,0.32f,1.0f,0xFFFF3030u}, {0.28f,0.0f,0.45f,0xFFFF3030u}, {-0.28f,0.0f,0.45f,0xFFFF3030u},
    {0.0f,0.32f,1.0f,0xFFFF3030u}, {-0.28f,0.0f,0.45f,0xFFFF3030u}, {0.0f,0.0f,0.2f,0xFFB02020u},
    /* engine glow */
    {0.0f,0.0f,-2.0f,0xFFFFFF40u}, {0.3f,0.0f,-2.35f,0xFFFFFF40u}, {-0.3f,0.0f,-2.35f,0xFFFFFF40u},
};

static void default_view(float *m)
{
    memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    m[14] = 8.0f;                       /* camera at z = -8, looking +z */
}

static void default_proj(float *m)
{
    float aspect = (float)g_fb_w / (float)(g_fb_h ? g_fb_h : 1);
    float f = 1.0f / tanf(0.5f * 1.04719755f);
    float zn = 0.1f, zf = 1000.0f;
    memset(m, 0, 64);
    m[0] = f / (aspect > 0.01f ? aspect : 1.0f);
    m[5] = f;
    m[10] = zf / (zf - zn);
    m[11] = 1.0f;
    m[14] = -zn * zf / (zf - zn);
}

/* True when the game has bound a VIEW transform (SetTransform path). */
int render_has_view(void)
{
    for (int i = 0; i < 16; i++)
        if (g_xform[0][i] != 0.0f)
            return 1;
    return 0;
}

void render_draw_ship(const float *world)
{
    static const float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float view[16], proj[16], wv[16], wvp[16], wc[16];
    const float *wp = world ? world : ident;
    int have_view = 0, have_proj = 0;
    uint32_t ntri = (uint32_t)(sizeof g_ship / sizeof g_ship[0]) / 3u;
    static int dbg;

    if (getenv("OTOMEDIUS_NO_SHIP") || !g_fb)
        return;
    for (int i = 0; i < 16; i++) {
        if (g_xform[0][i] != 0.0f) have_view = 1;
        if (g_xform[1][i] != 0.0f) have_proj = 1;
    }
    if (have_view) memcpy(view, g_xform[0], sizeof view); else default_view(view);
    if (have_proj) memcpy(proj, g_xform[1], sizeof proj); else default_proj(proj);
    /* The game's 3D scene uses a shader path whose VIEW transform we do not
     * capture yet; the container draw only gives us the node's world matrix,
     * whose translation is the ship's position in world space.  Until the
     * camera constants are wired up, centre that world matrix so the model is
     * visible instead of tens of units off-screen.  OTOMEDIUS_SHIP_CENTER=0
     * disables the override. */
    if (!have_view && !(getenv("OTOMEDIUS_SHIP_CENTER") &&
                        *getenv("OTOMEDIUS_SHIP_CENTER") == '0')) {
        memcpy(wc, wp, sizeof wc);
        wc[12] = wc[13] = wc[14] = 0.0f;
        wp = wc;
    }
    mat_mul_row(wv, wp, view);
    mat_mul_row(wvp, wv, proj);
    if (getenv("OTOMEDIUS_3DDBG") && dbg < 3) {
        dbg++;
        fprintf(stderr, "3ddbg: ship draw world=[%.3f %.3f %.3f %.3f | "
                "%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f] "
                "view=%d proj=%d effect=[%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | "
                "%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]\n",
                wp[0],wp[1],wp[2],wp[3],wp[4],wp[5],wp[6],
                wp[7],wp[8],wp[9],wp[10],wp[11],wp[12],wp[13],wp[14],wp[15],
                have_view, have_proj,
                g_effect_xform[0],g_effect_xform[1],g_effect_xform[2],g_effect_xform[3],
                g_effect_xform[4],g_effect_xform[5],g_effect_xform[6],g_effect_xform[7],
                g_effect_xform[8],g_effect_xform[9],g_effect_xform[10],g_effect_xform[11],
                g_effect_xform[12],g_effect_xform[13],g_effect_xform[14],g_effect_xform[15]);
    }
    for (uint32_t t = 0; t < ntri; t++) {
        svtx_t vt[3];
        int ok = 1;
        for (int k = 0; k < 3; k++) {
            const shipv_t *v = &g_ship[t * 3 + k];
            float cx = v->x*wvp[0] + v->y*wvp[4] + v->z*wvp[8]  + wvp[12];
            float cy = v->x*wvp[1] + v->y*wvp[5] + v->z*wvp[9]  + wvp[13];
            float cz = v->x*wvp[2] + v->y*wvp[6] + v->z*wvp[10] + wvp[14];
            float cw = v->x*wvp[3] + v->y*wvp[7] + v->z*wvp[11] + wvp[15];
            if (cw <= 1e-6f) { ok = 0; break; }
            vt[k].x = g_vp_x + (cx / cw + 1.0f) * 0.5f * g_vp_w;
            vt[k].y = g_vp_y + (1.0f - cy / cw) * 0.5f * g_vp_h;
            vt[k].z = cz / cw;
            vt[k].iw = 1.0f / cw;
            vt[k].u = vt[k].v = 0.0f;
            vt[k].color = v->color;
            vt[k].valid = 1;
        }
        if (ok)
            raster_array(vt, 3, 4, NULL, 0, 0, 1);
    }
}

/* Effect light constants.  The engine binds "LightDir" and "LightCol" through
 * ID3DXEffect::SetMatrix (see d3dx9.c), row 0 of each matrix being the first
 * directional light, so the model shading can use the scene's own light
 * instead of a hardcoded one. */
static float g_light_dir[3] = {-0.42f, 0.66f, -0.62f};
static float g_light_col[3] = {1.0f, 1.0f, 1.0f};

void render_set_effect_light(const char *which, const float *m)
{
    if (!which || !m)
        return;
    if (!strcmp(which, "LightDir")) {
        g_light_dir[0] = m[0];
        g_light_dir[1] = m[1];
        g_light_dir[2] = m[2];
    } else if (!strcmp(which, "LightCol")) {
        g_light_col[0] = m[0];
        g_light_col[1] = m[1];
        g_light_col[2] = m[2];
    }
}

/* The .x inside Player02.pak carries no TextureFilename entries; the engine
 * binds the pak textures per material in its own container draw.  Until that
 * path is replicated, bind the textures the port knows by name (see
 * texture_by_name) to the meshes they belong to; OTOMEDIUS_MODELTEX=<file.dds>
 * overrides the mapping for experiments. */
extern uint32_t texture_by_name(const char *name);

static uint32_t model_texture(int mesh)
{
    const char *n = xmodel_mesh_name(mesh);
    const char *ov = getenv("OTOMEDIUS_MODELTEX");
    if (ov && *ov)
        return texture_by_name(ov);
    if (!n)
        return 0;
    if (strstr(n, "bike"))
        return texture_by_name("p02_bike01.dds");
    if (strstr(n, "gun"))
        return texture_by_name("p02_gun01.dds");
    if (strstr(n, "body") || strstr(n, "hand"))
        return texture_by_name("p02_body01.dds");
    if (strstr(n, "transparence"))
        return texture_by_name("p02_hair.dds");
    return 0;
}

/* Draw a mesh parsed from the game's own .x file (see native/xmodel.c).
 *
 * The HLE container replaces the game's mesh container, so the texture stages
 * it would bind never reach this path: the model is shaded with the scene's
 * directional light and the material's diffuse colour instead.  `world` is the
 * node matrix the game passes to the container draw. */
void render_draw_model(const float *world, int mesh)
{
    static const float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float light[3] = {g_light_dir[0], g_light_dir[1], g_light_dir[2]};
    float view[16], proj[16], wv[16], wvp[16];
    const float *wp = world ? world : ident;
    const float *pos, *nrm, *uv;
    const uint32_t *idx, *fmat;
    const uint32_t *tpx = NULL;
    uint32_t tw2 = 0, th2 = 0;
    int nv, nf, have_view = 0, have_proj = 0;
    static int dbg;

    if (!g_fb || mesh < 0)
        return;
    g_stat.model_draws++;
    if (order_on())
        fprintf(stderr, "order %u MODEL %s present=%u\n", ++g_order_seq,
                xmodel_mesh_name(mesh), g_stat.presents);
    nv = xmodel_mesh_nverts(mesh);
    nf = xmodel_mesh_nfaces(mesh);
    pos = xmodel_mesh_pos(mesh);
    nrm = xmodel_mesh_norm(mesh);
    uv = xmodel_mesh_uv(mesh);
    idx = xmodel_mesh_idx(mesh);
    fmat = xmodel_mesh_facemat(mesh);
    if (nv <= 0 || nf <= 0 || !pos || !idx)
        return;
    {
        uint32_t mtex = model_texture(mesh);
        if (mtex)
            tpx = texture_argb(mtex, &tw2, &th2);
    }
    for (int i = 0; i < 16; i++) {
        if (g_xform[0][i] != 0.0f) have_view = 1;
        if (g_xform[1][i] != 0.0f) have_proj = 1;
    }
    /* The engine zeroes the device VIEW and hands that zero matrix to the
     * effect's "View" parameter, so the node matrices are already in camera
     * space: an absent VIEW means identity, not a fallback camera. */
    if (have_view) memcpy(view, g_xform[0], sizeof view); else memcpy(view, ident, sizeof view);
    if (have_proj) memcpy(proj, g_xform[1], sizeof proj); else default_proj(proj);
    mat_mul_row(wv, wp, view);
    mat_mul_row(wvp, wv, proj);
    if (getenv("OTOMEDIUS_3DDBG") && dbg < 4) {
        dbg++;
        fprintf(stderr, "3ddbg: model %s verts=%d faces=%d view=%d proj=%d "
                "world=[%.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | "
                "%.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f]\n",
                xmodel_mesh_name(mesh), nv, nf, have_view, have_proj,
                wp[0],wp[1],wp[2],wp[3], wp[4],wp[5],wp[6],wp[7],
                wp[8],wp[9],wp[10],wp[11], wp[12],wp[13],wp[14],wp[15]);
    }
    for (int f = 0; f < nf; f++) {
        svtx_t vt[3];
        float fn[3] = {0.0f, 0.0f, 0.0f};
        const float *mc;
        uint32_t mi = fmat ? fmat[f] : 0u;
        int ok = 1, i;

        mc = xmodel_mesh_matcolor(mesh, (int)mi);
        if (!mc)
            mc = xmodel_mesh_matcolor(mesh, 0);
        if (!nrm) {
            /* Face normal in world space, for models without MeshNormals. */
            const float *a = pos + idx[f * 3 + 0] * 3;
            const float *b = pos + idx[f * 3 + 1] * 3;
            const float *c = pos + idx[f * 3 + 2] * 3;
            float u[3] = {b[0]-a[0], b[1]-a[1], b[2]-a[2]};
            float v[3] = {c[0]-a[0], c[1]-a[1], c[2]-a[2]};
            fn[0] = u[1]*v[2] - u[2]*v[1];
            fn[1] = u[2]*v[0] - u[0]*v[2];
            fn[2] = u[0]*v[1] - u[1]*v[0];
        }
        for (i = 0; i < 3; i++) {
            uint32_t vi = idx[f * 3 + i];
            const float *p;
            float cx, cy, cz, cw, nx, ny, nz, nl, inten, r, g, b;
            if (vi >= (uint32_t)nv) {
                ok = 0;
                break;
            }
            p = pos + vi * 3;
            cx = p[0]*wvp[0] + p[1]*wvp[4] + p[2]*wvp[8]  + wvp[12];
            cy = p[0]*wvp[1] + p[1]*wvp[5] + p[2]*wvp[9]  + wvp[13];
            cz = p[0]*wvp[2] + p[1]*wvp[6] + p[2]*wvp[10] + wvp[14];
            cw = p[0]*wvp[3] + p[1]*wvp[7] + p[2]*wvp[11] + wvp[15];
            if (cw <= 1e-6f) {
                ok = 0;
                break;
            }
            if (nrm) {
                const float *n = nrm + vi * 3;
                nx = n[0]*wp[0] + n[1]*wp[4] + n[2]*wp[8];
                ny = n[0]*wp[1] + n[1]*wp[5] + n[2]*wp[9];
                nz = n[0]*wp[2] + n[1]*wp[6] + n[2]*wp[10];
            } else {
                nx = fn[0]; ny = fn[1]; nz = fn[2];
            }
            nl = nx*nx + ny*ny + nz*nz;
            if (nl > 1e-12f) {
                nl = 1.0f / sqrtf(nl);
                inten = nx*light[0] + ny*light[1] + nz*light[2];
                if (inten < 0.0f) inten = -inten;      /* two-sided */
                inten = 0.30f + 0.70f * (inten * nl);
            } else {
                inten = 0.6f;
            }
            r = mc[0] * inten * g_light_col[0];
            g = mc[1] * inten * g_light_col[1];
            b = mc[2] * inten * g_light_col[2];
            vt[i].x = g_vp_x + (cx / cw + 1.0f) * 0.5f * g_vp_w;
            vt[i].y = g_vp_y + (1.0f - cy / cw) * 0.5f * g_vp_h;
            vt[i].z = cz / cw;
            vt[i].iw = 1.0f / cw;
            vt[i].u = uv ? uv[vi * 2 + 0] : 0.0f;
            vt[i].v = uv ? uv[vi * 2 + 1] : 0.0f;
            vt[i].color = 0xFF000000u |
                ((uint32_t)(r >= 1.0f ? 255.0f : r * 255.0f) << 16) |
                ((uint32_t)(g >= 1.0f ? 255.0f : g * 255.0f) << 8) |
                (uint32_t)(b >= 1.0f ? 255.0f : b * 255.0f);
            vt[i].valid = 1;
        }
        if (ok) {
            g_stat.model_faces++;
            if (vt[0].z < 0.0f || vt[0].z > 1.0f ||
                vt[1].z < 0.0f || vt[1].z > 1.0f ||
                vt[2].z < 0.0f || vt[2].z > 1.0f)
                g_stat.model_zout++;
            for (i = 0; i < 3; i++) {
                int sx = (int)vt[i].x, sy = (int)vt[i].y;
                if (sx < g_stat.model_minx) g_stat.model_minx = sx;
                if (sx > g_stat.model_maxx) g_stat.model_maxx = sx;
                if (sy < g_stat.model_miny) g_stat.model_miny = sy;
                if (sy > g_stat.model_maxy) g_stat.model_maxy = sy;
            }
            g_in_model = 1;
            raster_array(vt, 3, 4, (uint32_t *)tpx, tw2, th2, 1);
            g_in_model = 0;
        } else {
            g_stat.model_clip++;
        }
    }
}

/* Experimental: drive the KonamiLogo manager directly.
 *
 * The boot/attract manager (0x4547E0) is supposed to call 0x439920 to set the
 * logo string and then 0x4395B0 to draw it, but its state machine crashes in
 * the CRT before it gets there.  The KonamiLogo manager itself (0x4393A0) is
 * created by the scene manager and is alive at [0x5DE23C], so call its load
 * and draw entry points from here.  OTOMEDIUS_KONAMI=1 enables it. */
static void konami_logo_tick(void)
{
    extern uint32_t x86_call_stdcall(uint32_t, const uint32_t *, int);
    cpu_t *cpu = x86_cpu();
    uint32_t mgr = M32(0x5DE23Cu);
    static int loaded;
    if (!getenv("OTOMEDIUS_KONAMI") || !mgr)
        return;
    if (!loaded) {
        loaded = 1;
        cpu->eax = 0;                 /* logo index 0 */
        x86_call_stdcall(0x439920u, NULL, 0);
        /* 0x439920 leaves the countdown at 0, and 0x439550 only calls the
         * atlas builder (0x4397C0) when the countdown reaches 0 *after* a
         * decrement.  Arm it so the next update builds the atlas. */
        M8(mgr + 0x28u) = 1;
        fprintf(stderr, "konami: logo string set\n");
    }
    cpu->eax = mgr;
    x86_call_stdcall(0x4395B0u, NULL, 0);
}

/* OTOMEDIUS_MOVIE=1 creates the game's movie-player manager - the object the
 * boot state machine builds at 0x454A4A (alloc 0xC + ctor 0x43F280) - so the
 * opening video can be exercised while the boot sequence is still stuck.  Its
 * update opens the DirectShow graph and its draw callback (0x43F540) samples
 * the video texture, which dshow_movie_tick() fills from the ffmpeg decoder. */
void dshow_movie_tick(void);

static void movie_manager_tick(void)
{
    extern uint32_t x86_call_stdcall(uint32_t, const uint32_t *, int);
    cpu_t *cpu = x86_cpu();
    static int done;
    uint32_t args[2];

    if (done || !getenv("OTOMEDIUS_MOVIE"))
        return;
    if (!M32(0x5E1F20u))            /* wait for the D3D device */
        return;
    done = 1;
    if (M32(0x5DE258u)) {
        fprintf(stderr, "movie: manager slot 0x5DE258 is busy (%08x)\n",
                M32(0x5DE258u));
        return;
    }
    args[0] = 0x5E1F28u;
    args[1] = 0x0Cu;
    cpu->eax = x86_call_stdcall(0x4142B0u, args, 2);
    if (!cpu->eax) {
        fprintf(stderr, "movie: local-heap alloc of the manager failed\n");
        return;
    }
    cpu->esi = cpu->eax;
    x86_call_stdcall(0x43F280u, NULL, 0);
    fprintf(stderr, "movie: player manager %08x, slot 0x5DE258=%08x\n",
            cpu->esi, M32(0x5DE258u));
}

/* Draw the movie the game's TextureRenderer produced.  Its own draw callback
 * (0x43F540) builds a degenerate quad in this port - the computed vertices are
 * (1,inf)-(2561,1024), i.e. the y scale divides by zero - so the port
 * composites the already-uploaded video texture into the software framebuffer
 * with the same fade colour the game uses ([0x5DE224], fully white during the
 * fade-in/out).  OTOMEDIUS_MOVIE_PREVIEW=1 shows the movie on its own, without
 * the game's manager. */
static void movie_draw_tick(void)
{
    static int preview_started;
    uint32_t wrap, filter, tex = 0, tw = 0, th = 0, fade, alpha, base;
    const uint32_t *px = NULL;
    const char *preview = getenv("OTOMEDIUS_MOVIE_PREVIEW");

    if (!g_fb)
        return;
    /* Diagnostic: OTOMEDIUS_NATIVE_MOVIE=1 leaves the movie to the game's own
     * draw callback (0x43F540) instead of compositing the texture here. */
    if (getenv("OTOMEDIUS_NATIVE_MOVIE"))
        return;
    if (preview) {
        if (!preview_started && M32(0x5E1F20u)) {
            char path[4096];
            preview_started = 1;
            snprintf(path, sizeof(path), "%s/Data/OTOMEDIUS_OP_f.wmv",
                     win_game_dir());
            if (movie_open(path, 640, 512, 30) != 0)
                fprintf(stderr, "movie: preview could not start the decoder\n");
        }
        if (movie_active() && movie_frame(g_fb, g_fb_w, g_fb_h))
            return;
    }
    /* Only while the game's movie-player manager owns slot 0x5DE258 (vtable
     * 0x55E904).  The startup probe (0x417480) also builds a graph and a
     * texture; without this check the video would flash during boot. */
    {
        uint32_t mgr = M32(0x5DE258u);
        if (!mgr || M32(mgr) != 0x55E904u)
            return;
    }
    if (!movie_active())
        return;
    wrap = M32(0x5DCF88u);
    filter = wrap ? M32(wrap + 0x10u) : 0;
    tex = filter ? M32(filter + 0x160u) : 0;
    if (!tex)
        return;
    px = texture_argb(tex, &tw, &th);
    if (!px || !tw || !th)
        return;
    fade = M32(0x5DE224u);
    alpha = fade >> 24;
    base = fade & 0x00FFFFFFu;
    if (!alpha && tw == (uint32_t)g_fb_w && th == (uint32_t)g_fb_h) {
        memcpy(g_fb, px, (size_t)g_fb_w * (size_t)g_fb_h * 4u);
        return;
    }
    /* [0x5DE224] is shared with the attract manager, which sets it opaque
     * white when it is constructed.  Only the movie player's own fade phases
     * (state 2 = fade in, state 4 = fade out) should dim the picture. */
    {
        uint32_t mgr = M32(0x5DE258u);
        uint32_t state = mgr ? M8(mgr + 0xAu) : 0;
        if (state != 2 && state != 4)
            alpha = 0;
    }
    for (int y = 0; y < g_fb_h; y++) {
        uint32_t sy = (uint32_t)((uint64_t)y * th / (uint32_t)g_fb_h);
        const uint32_t *s = px + (size_t)sy * tw;
        uint32_t *d = (uint32_t *)g_fb + (size_t)y * g_fb_w;
        for (int x = 0; x < g_fb_w; x++) {
            uint32_t tx = (uint32_t)((uint64_t)x * tw / (uint32_t)g_fb_w);
            uint32_t c = s[tx] | 0xFF000000u;
            if (alpha) {
                uint32_t r = (((c >> 16) & 0xFF) * (255 - alpha) +
                              ((base >> 16) & 0xFF) * alpha) / 255u;
                uint32_t g = (((c >> 8) & 0xFF) * (255 - alpha) +
                              ((base >> 8) & 0xFF) * alpha) / 255u;
                uint32_t b = ((c & 0xFF) * (255 - alpha) +
                              (base & 0xFF) * alpha) / 255u;
                c = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
            d[x] = c;
        }
    }
}

/* Renderer smoke test for the XYZ pipeline: OTOMEDIUS_TEST_3D=1 draws a
 * rotating cube with FVF XYZ|DIFFUSE through the same transform/raster path
 * the game's 3D draws use.  It proves W*V*P, /w, viewport and the z-buffer
 * without waiting for the .x model loader. */
static void test_3d_draw(void)
{
    extern uint32_t emu_alloc(uint32_t size);
    static uint32_t buf;
    static float ang;
    static const float cube[8][3] = {
        {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
        {-1,-1, 1},{1,-1, 1},{1,1, 1},{-1,1, 1}
    };
    static const uint8_t faces[12][3] = {
        {4,5,6},{4,6,7}, {1,0,3},{1,3,2}, {0,4,7},{0,7,3},
        {5,1,2},{5,2,6}, {3,7,6},{3,6,2}, {0,1,5},{0,5,4}
    };
    static const uint32_t fcol[6] = {
        0xFFFF4040u, 0xFF40FF40u, 0xFF4040FFu,
        0xFFFFFF40u, 0xFFFF40FFu, 0xFF40FFFFu
    };
    float ry[16], rx[16], world[16], view[16], proj[16];
    float sw[16], sv[16], sp[16];
    float c, s, f, aspect = (float)g_fb_w / (float)(g_fb_h ? g_fb_h : 1);
    const float zn = 0.1f, zf = 100.0f;
    uint32_t saved_fvf = g_fvf;

    ang += 0.02f;
    c = cosf(ang); s = sinf(ang);
    memset(ry, 0, sizeof ry); ry[0] = c; ry[2] = -s; ry[5] = 1.0f;
    ry[8] = s; ry[10] = c; ry[15] = 1.0f;
    memset(rx, 0, sizeof rx); rx[0] = 1.0f; rx[5] = c; rx[6] = -s;
    rx[9] = s; rx[10] = c; rx[15] = 1.0f;
    mat_mul_row(world, ry, rx);
    memset(view, 0, sizeof view);
    view[0] = view[5] = view[10] = view[15] = 1.0f;
    view[14] = 6.0f;                    /* camera at z = -6, looking +z */
    f = 1.0f / tanf(0.5f * 1.04719755f); /* 60 degrees */
    memset(proj, 0, sizeof proj);
    proj[0] = f / (aspect > 0.01f ? aspect : 1.0f);
    proj[5] = f;
    proj[10] = zf / (zf - zn);
    proj[11] = 1.0f;
    proj[14] = -zn * zf / (zf - zn);

    if (!buf)
        buf = emu_alloc(36u * 16u + 16u);
    for (int t = 0; t < 12; t++) {
        for (int k = 0; k < 3; k++) {
            uint32_t v = buf + (uint32_t)(t * 3 + k) * 16u;
            const float *p = cube[faces[t][k]];
            MF32(v + 0) = p[0];
            MF32(v + 4) = p[1];
            MF32(v + 8) = p[2];
            M32(v + 12) = fcol[t / 2];
        }
    }
    memcpy(sw, g_xform[2], sizeof sw);
    memcpy(sv, g_xform[0], sizeof sv);
    memcpy(sp, g_xform[1], sizeof sp);
    memcpy(g_xform[2], world, sizeof world);
    memcpy(g_xform[0], view, sizeof view);
    memcpy(g_xform[1], proj, sizeof proj);
    g_fvf = 0x042u;                     /* XYZ | DIFFUSE, stride 16 */
    draw_up_vertices(4, 12, buf, 16);
    memcpy(g_xform[2], sw, sizeof sw);
    memcpy(g_xform[0], sv, sizeof sv);
    memcpy(g_xform[1], sp, sizeof sp);
    g_fvf = saved_fvf;
}

void render_present(void)
{
    /* Diagnostic: OTOMEDIUS_QUIT_AT=n requests a clean quit at present n, so
     * the shutdown path can be exercised without a window. */
    if (getenv("OTOMEDIUS_QUIT_AT") &&
        g_stat.presents == (unsigned)atoi(getenv("OTOMEDIUS_QUIT_AT"))) {
        fprintf(stderr, "render: requesting quit at present %u\n",
                g_stat.presents);
        win_request_quit();
    }
    movie_manager_tick();
    dshow_movie_tick();
    konami_logo_tick();
    /* Renderer smoke test: OTOMEDIUS_TEST_QUAD=1 draws a full-screen quad with
     * the currently bound texture, through the same software path the game
     * uses.  It proves texture decode + raster + blend work without waiting for
     * the game to submit something visible. */
    if (getenv("OTOMEDIUS_TEST_QUAD") && g_fb && g_stat.presents < 400) {
        const char *mode = getenv("OTOMEDIUS_TEST_QUAD");
        extern uint32_t emu_alloc(uint32_t size);
        static uint32_t buf;
        const float quad[4][4] = {   /* x, y, u, v */
            {0.0f, 0.0f, 0.0f, 0.0f},
            {0.0f, (float)g_fb_h, 0.0f, 1.0f},
            {(float)g_fb_w, 0.0f, 1.0f, 0.0f},
            {(float)g_fb_w, (float)g_fb_h, 1.0f, 1.0f},
        };
        if (mode[0] == '2') {
            /* texture preview: prove the DDS/DXT decode, ignore alpha */
            uint32_t tw2 = 0, th2 = 0;
            const char *ts = getenv("OTOMEDIUS_TEST_TEX");
            uint32_t tobj = ts ? (uint32_t)strtoul(ts, NULL, 16) : g_tex0;
            const uint32_t *px = texture_argb(tobj, &tw2, &th2);
            if (g_verbose && !g_fb[0])
                fprintf(stderr, "render: preview tex %08x %ux%u\n", tobj, tw2,
                        th2);
            if (px && tw2 && th2) {
                for (int y = 0; y < g_fb_h; y++) {
                    for (int x = 0; x < g_fb_w; x++) {
                        uint32_t tx = (uint32_t)((uint64_t)x * tw2 / g_fb_w);
                        uint32_t ty = (uint32_t)((uint64_t)y * th2 / g_fb_h);
                        ((uint32_t *)g_fb)[(size_t)y * g_fb_w + x] =
                            px[(size_t)ty * tw2 + tx] | 0xFF000000u;
                    }
                }
            }
            goto test_done;
        }
        if (!buf)
            buf = emu_alloc(4 * 28 + 16);
        if (g_verbose) {
            uint32_t tw2 = 0, th2 = 0;
            const uint32_t *px = texture_argb(g_tex0, &tw2, &th2);
            fprintf(stderr, "render: test quad tex0=%08x %ux%u px=%p\n",
                    g_tex0, tw2, th2, (const void *)px);
        }
        for (int i = 0; i < 4; i++) {
            uint32_t v = buf + (uint32_t)i * 28;
            MF32(v + 0) = quad[i][0];
            MF32(v + 4) = quad[i][1];
            MF32(v + 8) = 0.0f;
            MF32(v + 12) = 1.0f;
            M32(v + 16) = 0xFFFFFFFFu;
            MF32(v + 20) = quad[i][2];
            MF32(v + 24) = quad[i][3];
        }
        draw_up_vertices(5, 2, buf, 28);
    }
    if (getenv("OTOMEDIUS_TEST_3D") && g_fb && g_stat.presents < 600)
        test_3d_draw();
    if (getenv("OTOMEDIUS_SHIP_OVERLAY") && g_fb)
        render_draw_ship(NULL);
test_done:
    movie_draw_tick();
    g_stat.presents++;
    if (g_verbose && (g_stat.presents % 120) == 0)
        fprintf(stderr,
                "render: frame %u: clear=%u draw=%u up=%u verts=%u "
                "fvf=%06x stride=%u vb=%08x tex0=%08x bbox=(%d,%d)-(%d,%d) "
                "onscreen=%u model=%u/%u mbox=(%d,%d)-(%d,%d) "
                "clip=%u zout=%u px=%lu/%lu target=%dx%d\n",
                g_stat.presents, g_stat.clears, g_stat.draws, g_stat.draw_ups,
                g_stat.verts, g_fvf, g_stream_stride, g_stream_vb, g_tex0,
                g_stat.minx, g_stat.miny, g_stat.maxx, g_stat.maxy,
                g_stat.onscreen, g_stat.model_draws, g_stat.model_faces,
                g_stat.model_minx, g_stat.model_miny,
                g_stat.model_maxx, g_stat.model_maxy,
                g_stat.model_clip, g_stat.model_zout,
                g_stat.px, g_stat.px_model, g_fb_w, g_fb_h);
    g_last_model_draws = g_stat.model_draws;
    g_stat.clears = g_stat.draws = g_stat.draw_ups = g_stat.verts = 0;
    g_stat.model_draws = g_stat.model_faces = 0;
    g_stat.model_clip = g_stat.model_zout = 0;
    g_stat.px = g_stat.px_model = 0;
    g_stat.model_minx = g_stat.model_miny = 0x7FFFFFFF;
    g_stat.model_maxx = g_stat.model_maxy = -0x7FFFFFFF;
    g_stat.minx = g_stat.miny = 0x7FFFFFFF;
    g_stat.maxx = g_stat.maxy = -0x7FFFFFFF;
    g_stat.onscreen = 0;

    /* OTOMEDIUS_DUMP_FB=<path.pnm> writes one frame (OTOMEDIUS_DUMP_AT, frame
     * 200 by default), which is how the headless build is checked without a
     * display. */
    {
        const char *dump = getenv("OTOMEDIUS_DUMP_FB");
        const char *at_s = getenv("OTOMEDIUS_DUMP_AT");
        unsigned at = at_s ? (unsigned)atoi(at_s) : 200u;
        const char *ev_s = getenv("OTOMEDIUS_DUMP_EVERY");
        unsigned ev = ev_s ? (unsigned)atoi(ev_s) : 0u;
        char dname[512];
        int hit = g_stat.presents == at;
        static int model_dumped;
        if (ev && g_stat.presents >= at && ((g_stat.presents - at) % ev) == 0) {
            snprintf(dname, sizeof dname, "%s.%05u", dump ? dump : "", g_stat.presents);
            dump = dname;
            hit = 1;
        }
        /* OTOMEDIUS_DUMP_MODEL=<path>: capture the first frame that drew the
         * 3D model, whatever present number that happens to be. */
        {
            const char *mp = getenv("OTOMEDIUS_DUMP_MODEL");
            if (mp && *mp && !model_dumped && g_last_model_draws) {
                dump = mp;
                hit = 1;
                model_dumped = 1;
                fprintf(stderr, "render: dumped model frame %u to %s\n",
                        g_stat.presents, mp);
            }
        }
        if (dump && *dump && hit)
            render_dump_fb(dump);
    }
    platform_present();
}

void render_clear(int count, uint32_t rects, uint32_t flags, uint32_t color)
{
    (void)count;
    (void)rects;
    g_stat.clears++;
    if (order_on())
        fprintf(stderr, "order %u CLEAR flags=%x color=%08x count=%d present=%u\n",
                ++g_order_seq, flags, color, count, g_stat.presents);
    if (flags & 1) {
        g_clear = color & 0x00FFFFFFu;
        fb_fill(g_clear);
    }
    if ((flags & 2) && g_zbuf) {
        for (size_t i = 0, n = (size_t)g_zb_w * g_zb_h; i < n; i++)
            g_zbuf[i] = 1.0f;
    }
    platform_clear(flags, color);
}

void render_set_viewport(int x, int y, int w, int h)
{
    if (w > 0 && h > 0) {
        g_vp_x = (float)x;
        g_vp_y = (float)y;
        g_vp_w = (float)w;
        g_vp_h = (float)h;
    }
    if (g_verbose > 1)
        fprintf(stderr, "render: viewport %d,%d %dx%d\n", x, y, w, h);
    platform_viewport(x, y, w, h);
}

void render_set_transform(uint32_t state, const void *matrix)
{
    /* Keep the fixed-function transforms: D3DTS_VIEW=2, PROJECTION=3,
     * WORLD=256.  They are needed by the XYZ (non-RHW) vertex path. */
    int slot = state == 2 ? 0 : state == 3 ? 1 : state == 256 ? 2 : -1;
    if (slot >= 0 && matrix)
        memcpy(g_xform[slot], matrix, 16 * sizeof(float));
    if (getenv("OTOMEDIUS_VPDBG") && slot >= 0) {
        static unsigned n;
        const float *f = (const float *)(slot >= 0 && matrix ? matrix : NULL);
        if (n++ < 60)
            fprintf(stderr, "render: SetTransform state=%u slot=%d [%s%.3f %.3f %.3f %.3f]\n",
                    state, slot, f ? "" : "(null) ",
                    f ? f[0] : 0.0f, f ? f[1] : 0.0f, f ? f[2] : 0.0f,
                    f ? f[3] : 0.0f);
    }
    platform_transform(state, matrix);
}

/* GetTransform: the game reads the device VIEW back (0x414e96) and feeds it to
 * the effect's "View" parameter, so this has to return the real state. */
int render_get_transform(uint32_t state, void *out)
{
    int slot = state == 2 ? 0 : state == 3 ? 1 : state == 256 ? 2 : -1;
    if (slot < 0 || !out)
        return 0;
    memcpy(out, g_xform[slot], 16 * sizeof(float));
    return 1;
}

void render_set_effect_matrix(const void *matrix)
{
    if (matrix)
        memcpy(g_effect_xform, matrix, sizeof g_effect_xform);
}

void render_set_state(uint32_t state, uint32_t value)
{
    if (state < 256)
        g_rs[state] = value;
    platform_state(state, value);
}

void render_set_sampler(uint32_t stage, uint32_t type, uint32_t value)
{
    if (stage == 0 && type < 16)
        g_ss[type] = value;
    platform_sampler(stage, type, value);
}

void render_set_texture(uint32_t stage, uint32_t tex)
{
    if (stage == 0) {
        g_tex0 = tex;
        if (getenv("OTOMEDIUS_TEXDBG")) {
            static unsigned n;
            extern unsigned render_presents(void);
            unsigned p = render_presents();
            if (n < 400 && p >= 4200 && p < 4400) {
                n++;
                fprintf(stderr, "render: SetTexture(0, %08x) present=%u\n",
                        tex, p);
            }
        }
    }
    platform_texture(stage, tex);
}

void render_set_stream(uint32_t stream, uint32_t vb, uint32_t offset, uint32_t stride)
{
    if (stream == 0) {
        g_stream_vb = vb;
        g_stream_stride = stride;
        g_stream_off = offset;
    }
    platform_stream(stream, vb, offset, stride);
}

void render_set_fvf(uint32_t fvf)
{
    g_fvf = fvf;
}

void render_set_indices(uint32_t ib, uint32_t base)
{
    g_ib = ib;
    g_ib_base = base;
    platform_indices(ib, base);
}

void render_set_pixel_shader(uint32_t ps)
{
    platform_pixel_shader(ps);
}

void render_draw(uint32_t prim, uint32_t base_vertex, uint32_t min_index,
                 uint32_t num_vertices, uint32_t start_index, int indexed,
                 uint32_t prim_count)
{
    g_stat.draws++;
    g_stat.verts += indexed ? prim_count : num_vertices;
    if (g_verbose > 1 && g_first_draw_logged < 8) {
        g_first_draw_logged++;
        fprintf(stderr,
                "render: draw prim=%u base=%u minv=%u nverts=%u start=%u "
                "idx=%d prims=%u | fvf=%06x stride=%u vb=%08x tex0=%08x\n",
                prim, base_vertex, min_index, num_vertices, start_index,
                indexed, prim_count, g_fvf, g_stream_stride, g_stream_vb,
                g_tex0);
    }
    draw_vb(prim, base_vertex, min_index, num_vertices, start_index, indexed,
            prim_count);
    platform_draw(prim, base_vertex, min_index, num_vertices, start_index,
                  indexed, prim_count);
}

void render_draw_up(uint32_t prim, uint32_t count, uint32_t data, uint32_t stride,
                    uint32_t fvf)
{
    (void)fvf;
    g_stat.draw_ups++;
    g_stat.verts += count;
    if (order_on())
        fprintf(stderr, "order %u UP prim=%u count=%u tex=%08x fvf=%06x present=%u\n",
                ++g_order_seq, prim, count, g_tex0, fvf, g_stat.presents);
    if (g_verbose > 1 && g_first_draw_logged < 8) {
        g_first_draw_logged++;
        fprintf(stderr,
                "render: draw_up prim=%u count=%u data=%08x stride=%u fvf=%06x\n",
                prim, count, data, stride, fvf);
    }
    draw_up_vertices(prim, count, data, stride);
    platform_draw_up(prim, count, data, stride, fvf);
}

unsigned render_presents(void) { return g_stat.presents; }
