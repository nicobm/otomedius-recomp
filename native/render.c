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
static uint32_t g_fvf, g_stream_stride, g_stream_vb, g_tex0;
static uint32_t g_first_draw_logged;

int render_width(void) { return g_width; }
int render_height(void) { return g_height; }

void render_init(int w, int h)
{
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
    if (area < 0)
        area = -area;

    x0 = (int)minx; y0 = (int)miny;
    x1 = (int)(maxx + 0.999f); y1 = (int)(maxy + 0.999f);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_fb_w) x1 = g_fb_w;
    if (y1 > g_fb_h) y1 = g_fb_h;

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

            col = 0xFFFFFFFFu;
            if (tex && tw && th) {
                /* default address mode is WRAP: u = 6.0 samples texel 0 */
                float uu = u - floorf(u);
                float vv = v - floorf(v);
                int tx = (int)(uu * (float)tw);
                int ty = (int)(vv * (float)th);
                if (tx < 0) tx = 0;
                if (ty < 0) ty = 0;
                if (tx >= (int)tw) tx = (int)tw - 1;
                if (ty >= (int)th) ty = (int)th - 1;
                col = tex[(size_t)ty * tw + tx];
            }
            {
                /* diffuse modulate */
                uint32_t dc = a->color;
                uint32_t dr = (dc >> 16) & 0xFF, dg = (dc >> 8) & 0xFF,
                         db = dc & 0xFF, da = (dc >> 24) & 0xFF;
                uint32_t r = (((col >> 16) & 0xFF) * dr) / 255;
                uint32_t g = (((col >> 8) & 0xFF) * dg) / 255;
                uint32_t b = ((col & 0xFF) * db) / 255;
                uint32_t al = (((col >> 24) & 0xFF) * da) / 255;
                fb_blend(x, y, (al << 24) | (r << 16) | (g << 8) | b);
            }
        }
    }
}

/* Rasterise an immediate vertex array (DrawPrimitiveUP). */
static void draw_up_vertices(uint32_t prim, uint32_t prim_count, uint32_t data,
                             uint32_t stride)
{
    const uint8_t *base;
    uint32_t nverts, *tex;
    uint32_t tw = 0, th = 0;
    svtx_t *vt;

    if (!g_fb || !data || !stride)
        return;
    switch (prim) {
    case 2: nverts = prim_count * 2; break;            /* line list */
    case 3: nverts = prim_count + 1; break;            /* line strip */
    case 4: nverts = prim_count * 3; break;            /* triangle list */
    case 5: nverts = prim_count + 2; break;            /* triangle strip */
    case 6: nverts = prim_count + 2; break;            /* triangle fan */
    default: return;                                   /* points: ignore */
    }
    if (nverts < 3 || nverts > 4096)
        return;

    vt = (svtx_t *)calloc(nverts, sizeof(*vt));
    if (!vt)
        return;
    base = MEM + data;
    for (uint32_t i = 0; i < nverts; i++) {
        const uint8_t *v = base + (size_t)i * stride;
        const float *f = (const float *)v;
        /* XYZRHW: x, y, z, rhw | DIFFUSE | TEX1: u, v  (stride 28) */
        vt[i].x = f[0];
        vt[i].y = f[1];
        vt[i].u = f[5];
        vt[i].v = f[6];
        vt[i].color = M32(data + i * stride + 16);
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
    if (g_tex0) {
        uint32_t w = 0, h = 0;
        const uint32_t *px = texture_argb(g_tex0, &w, &h);
        if (px) {
            tex = (uint32_t *)px;
            tw = w;
            th = h;
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

void render_present(void)
{
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
        if (dump && *dump && g_fb && g_stat.presents == at) {
            FILE *f = fopen(dump, "wb");
            if (f) {
                log_printf("render: dumped frame %u (%dx%d)", g_stat.presents,
                           g_fb_w, g_fb_h);
                fprintf(f, "P6\n%d %d\n255\n", g_fb_w, g_fb_h);
                for (size_t i = 0, n = (size_t)g_fb_w * g_fb_h; i < n; i++) {
                    uint32_t c = ((uint32_t *)g_fb)[i];
                    fputc((c >> 16) & 0xFF, f);
                    fputc((c >> 8) & 0xFF, f);
                    fputc(c & 0xFF, f);
                }
                fclose(f);
            }
        }
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
    platform_transform(state, matrix);
}

void render_set_state(uint32_t state, uint32_t value)
{
    platform_state(state, value);
}

void render_set_sampler(uint32_t stage, uint32_t type, uint32_t value)
{
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
