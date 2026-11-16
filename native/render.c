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

#include <string.h>
#include <stdio.h>

static int g_width = 1280, g_height = 1024;
static uint32_t g_clear = 0xFF000000u;

/* software frame buffer, 0xAARRGGBB (BGRA byte order, what SDL wants) */
static uint8_t *g_fb;
static int g_fb_w, g_fb_h;
static void fb_fill(uint32_t color);

/* ------------------------------------------------- renderer bring-up stats
 * Everything the game submits is counted per frame so a single log line says
 * whether the picture is missing because the game draws nothing or because the
 * platform backend throws it away. */
static struct {
    unsigned presents, clears, draws, draw_ups, verts;
    int minx, miny, maxx, maxy;   /* bounding box of the submitted quads */
    unsigned onscreen;
} g_stat;
static int g_cur_ntex;
static uint32_t g_fvf, g_stream_stride, g_stream_vb, g_tex0;
static uint32_t g_first_draw_logged;

int render_width(void) { return g_width; }
int render_height(void) { return g_height; }

static void render_state_defaults(void);

void render_init(int w, int h)
{
    render_state_defaults();
    if (w > 0 && h > 0) {
        g_width = (int)w;
        g_height = (int)h;
    }
    free(g_fb);
    g_fb_w = g_width;
    g_fb_h = g_height;
    g_fb = (uint8_t *)calloc((size_t)g_fb_w * g_fb_h, 4);
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
        *w = g_fb_w;
    if (h)
        *h = g_fb_h;
    return g_fb;
}

void render_dump_fb(const char *path)
{
    FILE *f;
    if (!path || !*path || !g_fb || g_fb_w <= 0 || g_fb_h <= 0)
        return;
    f = fopen(path, "wb");
    if (!f) {
        log_printf("render: cannot write dump %s", path);
        return;
    }
    log_printf("render: dumped frame %u (%dx%d) to %s", g_stat.presents,
               g_fb_w, g_fb_h, path);
    fprintf(f, "P6\n%d %d\n255\n", g_fb_w, g_fb_h);
    for (size_t i = 0, n = (size_t)g_fb_w * g_fb_h; i < n; i++) {
        uint32_t c = ((uint32_t *)g_fb)[i];
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
    uint32_t color;
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
enum { RS_ALPHATESTENABLE = 15, RS_SRCBLEND = 19, RS_DESTBLEND = 20,
       RS_ALPHAREF = 24, RS_ALPHAFUNC = 25, RS_ALPHABLENDENABLE = 27,
       RS_TEXTUREFACTOR = 60 };

static void render_state_defaults(void)
{
    memset(g_rs, 0, sizeof g_rs);
    g_rs[RS_SRCBLEND] = 2;           /* ONE */
    g_rs[RS_DESTBLEND] = 1;          /* ZERO */
    g_rs[RS_ALPHAFUNC] = 8;          /* ALWAYS */
    g_rs[RS_TEXTUREFACTOR] = 0xFFFFFFFFu;
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
                     const uint32_t *tex, uint32_t tw, uint32_t th)
{
    float minx = a->x, maxx = a->x, miny = a->y, maxy = a->y;
    float area;
    int x0, y0, x1, y1;

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
            uint32_t col;

            if (w0 < 0 && w1 < 0 && w2 < 0)
                continue;
            if (w0 > 0 && w1 > 0 && w2 > 0)
                continue;
            l0 = w0 / area; l1 = w1 / area; l2 = w2 / area;
            u = a->u * l0 + b->u * l1 + c->u * l2;
            v = a->v * l0 + b->v * l1 + c->v * l2;

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

static void draw_up_vertices(uint32_t prim, uint32_t prim_count, uint32_t data,
                             uint32_t stride)
{
    const uint8_t *base;
    uint32_t nverts, *tex;
    uint32_t tw = 0, th = 0;
    svtx_t *vt;

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
    if (nverts < 3 || nverts > 4096) {
        drawdbg_skip("nverts", prim, prim_count, data, stride, nverts);
        return;
    }

    vt = (svtx_t *)calloc(nverts, sizeof(*vt));
    if (!vt) {
        drawdbg_skip("calloc", prim, prim_count, data, stride, nverts);
        return;
    }
    base = MEM + data;
    {
        /* FVF layout: XYZRHW (x,y,z,rhw) | DIFFUSE | SPECULAR | TEXn */
        uint32_t fvf = g_fvf ? g_fvf : 0x144u;
        uint32_t off_diff = 16, off_tex = 16;
        int has_diff = (fvf & 0x40) != 0, ntex = (int)((fvf >> 8) & 15);
        if (has_diff) off_tex += 4;
        if (fvf & 0x80) off_tex += 4;
        if (!(fvf & 4) || (ntex == 0 && stride < 20)) {
            static int dbg = -2;
            static unsigned nd;
            if (dbg == -2)
                dbg = getenv("OTOMEDIUS_DRAWDBG") ? 1 : 0;
            if (dbg && nd < 40) {
                const float *f = (const float *)(base);
                nd++;
                fprintf(stderr, "drawdbg[%u]: DISCARD prim=%u n=%u fvf=%x "
                        "stride=%u v0=(%.3f,%.3f,%.3f,%.3f) v1=(%.3f,%.3f,%.3f,%.3f)\n",
                        g_stat.presents, prim, nverts, fvf, stride,
                        f[0], f[1], f[2], f[3],
                        f[stride / 4], f[stride / 4 + 1], f[stride / 4 + 2],
                        f[stride / 4 + 3]);
                if (nd <= 2) {
                    static const char *nm[3] = { "WORLD", "VIEW", "PROJ" };
                    for (int s = 0; s < 3; s++) {
                        const float *m = g_xform[s];
                        fprintf(stderr, "  %s [%.4f %.4f %.4f %.4f | "
                                "%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | "
                                "%.4f %.4f %.4f %.4f]\n", nm[s],
                                m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
                                m[8], m[9], m[10], m[11], m[12], m[13], m[14],
                                m[15]);
                    }
                }
            }
            free(vt);
            return;
        }
        for (uint32_t i = 0; i < nverts; i++) {
            const uint8_t *v = base + (size_t)i * stride;
            const float *f = (const float *)v;
            vt[i].x = f[0];
            vt[i].y = f[1];
            vt[i].color = has_diff ? M32(data + i * stride + off_diff) : 0xFFFFFFFFu;
            if (ntex > 0) {
                const float *t = (const float *)(v + off_tex);
                vt[i].u = t[0];
                vt[i].v = t[1];
            }
        }
        g_cur_ntex = ntex;
    }
    if (getenv("OTOMEDIUS_VERTDBG")) {
        /* print the first draw of every 120th frame so the coordinates can be
         * compared against the expected layout */
        static unsigned frame_no;
        if ((frame_no++ % 120) == 0) {
            fprintf(stderr, "render: verts prim=%u n=%u stride=%u:", prim,
                    nverts, stride);
            for (uint32_t i = 0; i < nverts && i < 4; i++)
                fprintf(stderr, " (%.1f,%.1f uv=%.2f,%.2f)", vt[i].x, vt[i].y,
                        vt[i].u, vt[i].v);
            fprintf(stderr, "  fb=%dx%d\n", g_fb_w, g_fb_h);
        }
    }
    if (g_verbose > 2) {
        static int dbg;
        if (dbg < 2) {
            dbg++;
            fprintf(stderr, "render: verts prim=%u n=%u stride=%u:",
                    prim, nverts, stride);
            for (uint32_t i = 0; i < nverts && i < 32; i++) {
                const uint32_t *w32 = (const uint32_t *)(base + (size_t)i * stride);
                fprintf(stderr, "\n   v%u:", i);
                for (uint32_t k = 0; k < stride / 4; k++)
                    fprintf(stderr, " %08x", w32[k]);
            }
            fprintf(stderr, "\n");
        }
    }

    /* The game's font renderer passes texture coordinates in *cell* units:
     * a glyph at column 6, row 4 arrives as u=6.0, v=4.0, and the quad's
     * second corner is u=6.09375 (one cell).  Treating those as normalised
     * UVs makes WRAP sample texel 0 for every glyph.  Detect that convention
     * (out-of-range integer UVs with a sub-cell span) and expand the integer
     * part with the cell size taken from this quad. */
    {
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

    tex = NULL;
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
                    "rs(ab=%u src=%u dst=%u at=%u) tss(cop=%u c1=%u c2=%u aop=%u) "
                    "ss(u=%u v=%u mag=%u) v0=(%.0f,%.0f uv %.3f,%.3f c=%08x) "
                    "v2=(%.0f,%.0f uv %.3f,%.3f)\n",
                    g_stat.presents, prim, nverts, g_fvf, stride, g_tex0, tw, th,
                    g_rs[RS_ALPHABLENDENABLE], g_rs[RS_SRCBLEND], g_rs[RS_DESTBLEND],
                    g_rs[RS_ALPHATESTENABLE], g_tss[1], g_tss[2], g_tss[3], g_tss[4],
                    g_ss[1], g_ss[2], g_ss[5], vt[0].x, vt[0].y, vt[0].u, vt[0].v,
                    vt[0].color, vt[nverts > 2 ? 2 : 0].x, vt[nverts > 2 ? 2 : 0].y,
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

    if (prim == 4) {
        for (uint32_t i = 0; i + 2 < nverts; i += 3)
            draw_tri(&vt[i], &vt[i + 1], &vt[i + 2], tex, tw, th);
    } else if (prim == 5) {
        for (uint32_t i = 0; i + 2 < nverts; i++) {
            if (i & 1)
                draw_tri(&vt[i + 1], &vt[i], &vt[i + 2], tex, tw, th);
            else
                draw_tri(&vt[i], &vt[i + 1], &vt[i + 2], tex, tw, th);
        }
    } else if (prim == 6) {
        for (uint32_t i = 1; i + 1 < nverts; i++)
            draw_tri(&vt[0], &vt[i], &vt[i + 1], tex, tw, th);
    } else {
        drawdbg_skip("prim-draw", prim, prim_count, data, stride, nverts);
    }
    free(vt);
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
test_done:
    movie_draw_tick();
    g_stat.presents++;
    if (g_verbose && (g_stat.presents % 120) == 0)
        fprintf(stderr,
                "render: frame %u: clear=%u draw=%u up=%u verts=%u "
                "fvf=%06x stride=%u vb=%08x tex0=%08x bbox=(%d,%d)-(%d,%d) "
                "onscreen=%u\n",
                g_stat.presents, g_stat.clears, g_stat.draws, g_stat.draw_ups,
                g_stat.verts, g_fvf, g_stream_stride, g_stream_vb, g_tex0,
                g_stat.minx, g_stat.miny, g_stat.maxx, g_stat.maxy,
                g_stat.onscreen);
    g_stat.clears = g_stat.draws = g_stat.draw_ups = g_stat.verts = 0;
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
        if (ev && g_stat.presents >= at && ((g_stat.presents - at) % ev) == 0) {
            snprintf(dname, sizeof dname, "%s.%05u", dump ? dump : "", g_stat.presents);
            dump = dname;
            hit = 1;
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
    if (flags & 1) {
        g_clear = color & 0x00FFFFFFu;
        fb_fill(g_clear);
    }
    platform_clear(flags, color);
}

void render_set_viewport(int x, int y, int w, int h)
{
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
    platform_transform(state, matrix);
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
    if (stage == 0)
        g_tex0 = tex;
    platform_texture(stage, tex);
}

void render_set_stream(uint32_t stream, uint32_t vb, uint32_t offset, uint32_t stride)
{
    if (stream == 0) {
        g_stream_vb = vb;
        g_stream_stride = stride;
    }
    platform_stream(stream, vb, offset, stride);
}

void render_set_fvf(uint32_t fvf)
{
    g_fvf = fvf;
}

void render_set_indices(uint32_t ib, uint32_t base)
{
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
    platform_draw(prim, base_vertex, min_index, num_vertices, start_index,
                  indexed, prim_count);
}

void render_draw_up(uint32_t prim, uint32_t count, uint32_t data, uint32_t stride,
                    uint32_t fvf)
{
    (void)fvf;
    g_stat.draw_ups++;
    g_stat.verts += count;
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
