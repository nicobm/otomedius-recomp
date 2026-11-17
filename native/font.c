/* font.c - FreeType-backed GDI font/glyph backend.
 *
 * The game's CTexFont asks GDI for real glyph outlines (GGO_NATIVE) and
 * rasterises them into its own atlases.  The old synthetic path returned a
 * rectangle for every character, which produced a fragmented, unreadable
 * atlas.  This module loads a system TrueType/OpenType font with FreeType,
 * returns the genuine outlines/metrics and flattens curves to line segments
 * so the game's rasteriser does not have to understand QSPLINE/CSPLINE.
 *
 * FreeType is optional: when it (or a usable font) is missing every entry
 * point returns 0 and win32_misc.c falls back to the synthetic implementation.
 *
 * The game also logs from the async loader thread, so the same FT_Face can be
 * used by the main and loader threads.  FreeType faces are not reentrant, so
 * every public entry point is protected by a recursive mutex.
 */
#define _GNU_SOURCE
#include "win32.h"
#include "font.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_FONTS        16
#define FONT_HANDLE_BASE 0x0F010000u
#define CONTOUR_MAX      4096

static pthread_mutex_t g_lock;
static pthread_once_t  g_lock_once = PTHREAD_ONCE_INIT;

static void init_lock(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_lock, &a);
    pthread_mutexattr_destroy(&a);
}

#define FONT_LOCK()   do { pthread_once(&g_lock_once, init_lock); \
                           pthread_mutex_lock(&g_lock); } while (0)
#define FONT_UNLOCK() pthread_mutex_unlock(&g_lock)

typedef struct {
    uint32_t handle;
    FT_Face  face;
    int      height;
    int      used;
} font_slot_t;

static FT_Library   g_lib;
static int          g_init_done;
static int          g_ok;
static font_slot_t  g_fonts[MAX_FONTS];
static int          g_current = -1;
static char         g_font_path[512];

static int file_exists(const char *p)
{
    return p && *p && access(p, R_OK) == 0;
}

static int try_fc_match(const char *pattern, char *out, size_t n)
{
    char cmd[512];
    FILE *p;
    size_t l;

    snprintf(cmd, sizeof(cmd), "fc-match -f '%%{file}' '%s' 2>/dev/null",
             pattern);
    p = popen(cmd, "r");
    if (!p)
        return 0;
    out[0] = 0;
    if (!fgets(out, (int)n, p))
        out[0] = 0;
    pclose(p);
    l = strlen(out);
    while (l && (out[l - 1] == '\n' || out[l - 1] == '\r'))
        out[--l] = 0;
    return file_exists(out);
}

static const char *pick_font(void)
{
    static char tmp[512];
    static const char *cand[] = {
        /* Prefer a TrueType font: the game's native-outline rasteriser
         * understands TT_PRIM_QSPLINE easily and TrueType has no cubics.
         * A CJK font can be forced with OTOMEDIUS_FONT later. */
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/gnu-free/FreeSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf",
        "/usr/share/fonts/truetype/ipafont-gothic/ipag.ttf",
        "/usr/share/fonts/truetype/fonts-japanese-gothic.ttf",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/run/host/fonts/google-noto-sans-cjk-vf-fonts/NotoSansCJK-VF.ttc",
        NULL
    };
    const char *env = getenv("OTOMEDIUS_FONT");
    int i;

    if (file_exists(env))
        return env;

    if (try_fc_match("sans", tmp, sizeof(tmp)))
        return tmp;

    for (i = 0; cand[i]; i++)
        if (file_exists(cand[i]))
            return cand[i];
    return NULL;
}

void font_init(void)
{
    const char *p;

    FONT_LOCK();
    if (g_init_done) {
        FONT_UNLOCK();
        return;
    }
    g_init_done = 1;
    if (getenv("OTOMEDIUS_NO_FREETYPE")) {
        FONT_UNLOCK();
        return;
    }
    if (FT_Init_FreeType(&g_lib) != 0) {
        fprintf(stderr, "font: FT_Init_FreeType failed\n");
        FONT_UNLOCK();
        return;
    }
    p = pick_font();
    if (!p) {
        fprintf(stderr, "font: no usable TTF/OTF found "
                "(set OTOMEDIUS_FONT=/path/to/font.ttf)\n");
        FT_Done_FreeType(g_lib);
        g_lib = NULL;
        FONT_UNLOCK();
        return;
    }
    snprintf(g_font_path, sizeof(g_font_path), "%s", p);
    g_ok = 1;
    if (g_verbose)
        fprintf(stderr, "font: using %s\n", g_font_path);
    FONT_UNLOCK();
}

void font_shutdown(void)
{
    int i;
    FONT_LOCK();
    for (i = 0; i < MAX_FONTS; i++) {
        if (g_fonts[i].used && g_fonts[i].face) {
            FT_Done_Face(g_fonts[i].face);
            g_fonts[i].face = NULL;
            g_fonts[i].used = 0;
        }
    }
    if (g_lib)
        FT_Done_FreeType(g_lib);
    g_lib = NULL;
    g_ok = 0;
    g_init_done = 0;
    FONT_UNLOCK();
}

int font_available(void)
{
    int ok;
    font_init();
    FONT_LOCK();
    ok = g_ok;
    FONT_UNLOCK();
    return ok;
}

/* The boot pak carries its own font file and registers it with
 * AddFontMemResourceEx.  Keeping a memory face lets the port draw with the
 * arcade typeface instead of a system substitute. */
static const unsigned char *g_mem_font;
static unsigned long        g_mem_font_size;

int font_add_memory(const void *data, unsigned long size)
{
    FT_Face f = NULL;
    int ok = 0;

    if (!data || size < 12)
        return 0;
    FONT_LOCK();
    font_init();
    if (!g_ok) {
        FONT_UNLOCK();
        return 0;
    }
    if (FT_New_Memory_Face(g_lib, (const FT_Byte *)data, (FT_Long)size, 0,
                           &f) == 0) {
        g_mem_font = (const unsigned char *)data;
        g_mem_font_size = size;
        ok = 1;
        if (g_verbose)
            fprintf(stderr, "font: registered memory font (%lu bytes, %s)\n",
                    size, f->family_name ? f->family_name : "?");
        FT_Done_Face(f);
    } else if (g_verbose) {
        fprintf(stderr, "font: AddFontMemResourceEx blob is not a font "
                "(first bytes %02x %02x %02x %02x)\n",
                ((const unsigned char *)data)[0],
                ((const unsigned char *)data)[1],
                ((const unsigned char *)data)[2],
                ((const unsigned char *)data)[3]);
    }
    FONT_UNLOCK();
    return ok;
}

static FT_Face open_face(int height)
{
    static const char *cand[] = {
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/gnu-free/FreeSans.ttf",
        "/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf",
        "/usr/share/fonts/truetype/ipafont-gothic/ipag.ttf",
        "/usr/share/fonts/truetype/fonts-japanese-gothic.ttf",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/run/host/fonts/google-noto-sans-cjk-vf-fonts/NotoSansCJK-VF.ttc",
        NULL
    };
    const char *paths[MAX_FONTS + 8];
    char tmp[512];
    int n = 0, i;

    /* The game's own font first, when it registered one. */
    if (g_mem_font && !getenv("OTOMEDIUS_SYSFONT")) {
        FT_Face f = NULL;
        int px = (height + 3) / 4;   /* CTexFont stores glyphs at 1/4 size */
        if (px < 4)
            px = 4;
        if (FT_New_Memory_Face(g_lib, g_mem_font, (FT_Long)g_mem_font_size, 0,
                               &f) == 0) {
            if (FT_Set_Pixel_Sizes(f, 0, (FT_UInt)px) == 0)
                return f;
            FT_Done_Face(f);
        }
    }

    if (g_font_path[0])
        paths[n++] = g_font_path;
    if (try_fc_match("sans", tmp, sizeof(tmp)))
        paths[n++] = tmp;
    for (i = 0; cand[i] && n < (int)(sizeof(paths) / sizeof(paths[0])); i++)
        paths[n++] = cand[i];

    for (i = 0; i < n; i++) {
        FT_Face f = NULL;
        if (!file_exists(paths[i]))
            continue;
        int px = (height + 3) / 4;   /* CTexFont stores glyphs at 1/4 size */
        if (px < 4)
            px = 4;
        if (FT_New_Face(g_lib, paths[i], 0, &f) != 0)
            continue;
        if (FT_Set_Pixel_Sizes(f, 0, (FT_UInt)px) != 0) {
            FT_Done_Face(f);
            continue;
        }
        snprintf(g_font_path, sizeof(g_font_path), "%s", paths[i]);
        return f;
    }
    return NULL;
}

uint32_t font_create(int height, const char *face)
{
    FT_Face f;
    int i;

    FONT_LOCK();
    font_init();
    if (!g_ok) {
        FONT_UNLOCK();
        return 0;
    }
    if (height < 0)
        height = -height;
    if (height < 6)
        height = 6;
    if (height > 256)
        height = 256;

    /* the game creates the same pixel size again and again: reuse it */
    for (i = 0; i < MAX_FONTS; i++) {
        if (g_fonts[i].used && g_fonts[i].height == height) {
            uint32_t h = g_fonts[i].handle;
            FONT_UNLOCK();
            return h;
        }
    }

    f = open_face(height);
    if (!f) {
        fprintf(stderr, "font: could not open a font for h=%d\n", height);
        FONT_UNLOCK();
        return 0;
    }

    for (i = 0; i < MAX_FONTS; i++) {
        if (g_fonts[i].used)
            continue;
        g_fonts[i].used = 1;
        g_fonts[i].face = f;
        g_fonts[i].height = height;
        g_fonts[i].handle = FONT_HANDLE_BASE + (uint32_t)i;
        if (g_verbose) {
            char shown[128];
            fprintf(stderr, "font: CreateFontA(h=%d face=%s) -> %08x\n",
                    height, face ? win_sjis_to_utf8(face, shown, sizeof(shown))
                                 : "?", g_fonts[i].handle);
        }
        FONT_UNLOCK();
        return g_fonts[i].handle;
    }
    FT_Done_Face(f);
    FONT_UNLOCK();
    return 0;
}

static int slot_for(uint32_t handle)
{
    int i;
    for (i = 0; i < MAX_FONTS; i++)
        if (g_fonts[i].used && g_fonts[i].handle == handle)
            return i;
    return -1;
}

void font_select(uint32_t handle)
{
    int i;
    FONT_LOCK();
    i = slot_for(handle);
    if (i >= 0)
        g_current = i;
    FONT_UNLOCK();
}

void font_destroy(uint32_t handle)
{
    int i;
    FONT_LOCK();
    i = slot_for(handle);
    if (i < 0) {
        FONT_UNLOCK();
        return;
    }
    if (g_fonts[i].face)
        FT_Done_Face(g_fonts[i].face);
    g_fonts[i].face = NULL;
    g_fonts[i].used = 0;
    if (g_current == i)
        g_current = -1;
    FONT_UNLOCK();
}

static FT_Face current_face(uint32_t handle)
{
    int i = handle ? slot_for(handle) : -1;
    if (i < 0)
        i = g_current;
    if (i < 0) {
        uint32_t h = font_create(24, NULL);
        i = slot_for(h);
    }
    return i >= 0 ? g_fonts[i].face : NULL;
}

/* --------------------------------------------------------------- metrics */
int font_text_metrics(uint32_t handle, uint32_t tm)
{
    FT_Face f;
    FT_Size_Metrics *m;
    int h, asc, desc, lead, maxw, ave;

    FONT_LOCK();
    f = current_face(handle);
    if (!f || !f->size || !tm) {
        FONT_UNLOCK();
        return 0;
    }
    m = &f->size->metrics;
    h    = (int)((m->height + 63) >> 6);
    asc  = (int)((m->ascender + 63) >> 6);
    desc = (int)((-m->descender + 63) >> 6);
    lead = h - asc - desc;
    if (lead < 0)
        lead = 0;
    maxw = (int)((m->max_advance + 63) >> 6);
    ave  = maxw;
    if (FT_Load_Char(f, 'x', FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING) == 0)
        ave = (int)((f->glyph->advance.x + 32) >> 6);

    memset(MEM + tm, 0, 56);
    M32(tm + 0)  = (uint32_t)h;
    M32(tm + 4)  = (uint32_t)asc;
    M32(tm + 8)  = (uint32_t)desc;
    M32(tm + 12) = (uint32_t)lead;
    M32(tm + 16) = 0;                  /* tmExternalLeading */
    M32(tm + 20) = (uint32_t)ave;      /* tmAveCharWidth    */
    M32(tm + 24) = (uint32_t)maxw;     /* tmMaxCharWidth    */
    M32(tm + 28) = 400;                /* tmWeight          */
    M32(tm + 32) = 0;                  /* tmOverhang        */
    M32(tm + 36) = 96;                 /* tmDigitizedAspectX */
    M32(tm + 40) = 96;                 /* tmDigitizedAspectY */
    M8(tm + 44)  = 0x20;               /* tmFirstChar       */
    M8(tm + 45)  = 0xff;               /* tmLastChar        */
    M8(tm + 46)  = 0x3f;               /* tmDefaultChar     */
    M8(tm + 47)  = 0x20;               /* tmBreakChar       */
    M8(tm + 48)  = 0;                  /* tmItalic          */
    M8(tm + 49)  = 0;                  /* tmUnderlined      */
    M8(tm + 50)  = 0;                  /* tmStruckOut       */
    M8(tm + 51)  = 0;                  /* tmPitchAndFamily  */
    M8(tm + 52)  = 0;                  /* tmCharSet (ANSI)  */
    FONT_UNLOCK();
    return 1;
}

int font_abc_widths(uint32_t handle, uint32_t first, uint32_t last, uint32_t abc)
{
    FT_Face f;
    uint32_t c;

    FONT_LOCK();
    f = current_face(handle);
    if (!f || !abc || last < first || last - first > 0xFFFF) {
        FONT_UNLOCK();
        return 0;
    }
    for (c = first; c <= last; c++) {
        FT_UInt gi = FT_Get_Char_Index(f, c);
        int a = 0, b = 0, cc = 0;
        uint32_t p = abc + (c - first) * 12;
        if (FT_Load_Glyph(f, gi, FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING) == 0) {
            int adv = (int)(f->glyph->advance.x >> 6);
            a = (int)(f->glyph->metrics.horiBearingX >> 6);
            b = (int)(f->glyph->metrics.width >> 6);
            cc = adv - a - b;
        }
        /* Windows ABC is three 32-bit fields (12 bytes).  The game reads
         * abcA/abcB/abcC as dwords right after the call. */
        M32(p + 0) = (uint32_t)a;
        M32(p + 4) = (uint32_t)b;
        M32(p + 8) = (uint32_t)cc;
    }
    FONT_UNLOCK();
    return 1;
}

/* ------------------------------------------------------------- outlines */
typedef struct {
    uint8_t *p;
    size_t   n, cap;
    int      oom;
} obuf_t;

static int  ob_room(obuf_t *b, size_t add);
static void ob_u16(obuf_t *b, uint16_t v);
static void ob_u32(obuf_t *b, uint32_t v);
static void ob_point(obuf_t *b, FT_Pos x, FT_Pos y);

static int ob_room(obuf_t *b, size_t add)
{
    size_t nc;
    uint8_t *np;
    if (b->n + add <= b->cap)
        return 1;
    nc = b->cap ? b->cap : 256;
    while (nc < b->n + add)
        nc *= 2;
    np = realloc(b->p, nc);
    if (!np) {
        b->oom = 1;
        return 0;
    }
    b->p = np;
    b->cap = nc;
    return 1;
}

static void ob_u16(obuf_t *b, uint16_t v)
{
    if (!ob_room(b, 2)) return;
    b->p[b->n++] = (uint8_t)(v & 0xFF);
    b->p[b->n++] = (uint8_t)(v >> 8);
}

static void ob_u32(obuf_t *b, uint32_t v)
{
    if (!ob_room(b, 4)) return;
    b->p[b->n++] = (uint8_t)(v & 0xFF);
    b->p[b->n++] = (uint8_t)((v >> 8) & 0xFF);
    b->p[b->n++] = (uint8_t)((v >> 16) & 0xFF);
    b->p[b->n++] = (uint8_t)((v >> 24) & 0xFF);
}

static void ob_point(obuf_t *b, FT_Pos x, FT_Pos y)
{
    /* FreeType outlines are 26.6; GDI POINTFX is 16.16. */
    ob_u32(b, (uint32_t)(int32_t)((int64_t)x * 1024));
    ob_u32(b, (uint32_t)(int32_t)((int64_t)y * 1024));
}

#define LINE_CHUNK 8

typedef struct {
    obuf_t *b;
    size_t  start;              /* offset of the contour header */
    FT_Pos  sx, sy;             /* pfxStart */
    FT_Pos  px, py;             /* previous on-curve point */
    FT_Pos  lx[LINE_CHUNK], ly[LINE_CHUNK];
    int     ln;                 /* pending line points */
    int     open;
} contour_t;

/* Emit the pending run of line segments as one TT_PRIM_LINE curve. */
static void line_flush(contour_t *c)
{
    int i;
    if (!c->open || c->ln <= 0) {
        c->ln = 0;
        return;
    }
    ob_u16(c->b, 1);            /* TT_PRIM_LINE */
    ob_u16(c->b, (uint16_t)c->ln);
    for (i = 0; i < c->ln; i++)
        ob_point(c->b, c->lx[i], c->ly[i]);
    c->px = c->lx[c->ln - 1];
    c->py = c->ly[c->ln - 1];
    c->ln = 0;
}

static void line_add(contour_t *c, FT_Pos x, FT_Pos y)
{
    if (c->ln >= LINE_CHUNK)
        line_flush(c);
    c->lx[c->ln] = x;
    c->ly[c->ln] = y;
    c->ln++;
}

/* Close the current contour and patch its cb. */
static void contour_finish(contour_t *c)
{
    uint32_t cb;
    if (!c->open)
        return;
    line_flush(c);
    if (c->px != c->sx || c->py != c->sy) {
        line_add(c, c->sx, c->sy);
        line_flush(c);
    }
    cb = (uint32_t)(c->b->n - c->start);
    if (c->b->p && c->start + 4 <= c->b->n) {
        c->b->p[c->start + 0] = (uint8_t)(cb & 0xFF);
        c->b->p[c->start + 1] = (uint8_t)((cb >> 8) & 0xFF);
        c->b->p[c->start + 2] = (uint8_t)((cb >> 16) & 0xFF);
        c->b->p[c->start + 3] = (uint8_t)((cb >> 24) & 0xFF);
    }
    c->open = 0;
}

static void contour_begin(contour_t *c, FT_Pos x, FT_Pos y)
{
    contour_finish(c);
    c->start = c->b->n;
    ob_u32(c->b, 0);            /* cb, patched by contour_finish */
    ob_u32(c->b, 24);           /* TT_POLYGON_TYPE */
    ob_point(c->b, x, y);
    c->sx = c->px = x;
    c->sy = c->py = y;
    c->ln = 0;
    c->open = 1;
}

static int cb_move(const FT_Vector *to, void *ud)
{
    contour_t *c = (contour_t *)ud;
    contour_begin(c, to->x, to->y);
    return 0;
}

static int cb_line(const FT_Vector *to, void *ud)
{
    contour_t *c = (contour_t *)ud;
    if (!c->open)
        contour_begin(c, to->x, to->y);
    else
        line_add(c, to->x, to->y);
    return 0;
}

/* Flatten a quadratic into short line segments.  The game's CTexFont
 * rasteriser is a line scanner; it copes with the synthetic 4-point line
 * contour but not with a single huge curve or QSPLINE. */
static int cb_conic(const FT_Vector *ctrl, const FT_Vector *to, void *ud)
{
    contour_t *c = (contour_t *)ud;
    const int N = 8;
    FT_Pos x0, y0;
    int i;

    if (!c->open)
        contour_begin(c, to->x, to->y);
    if (c->ln < 1 && c->px == c->sx && c->py == c->sy) {
        /* first segment: current point is pfxStart */
    }
    x0 = c->px;
    y0 = c->py;
    for (i = 1; i <= N; i++) {
        double t = (double)i / N, u = 1.0 - t;
        double x = u * u * x0 + 2.0 * u * t * ctrl->x + t * t * to->x;
        double y = u * u * y0 + 2.0 * u * t * ctrl->y + t * t * to->y;
        line_add(c, (FT_Pos)(x + (x >= 0 ? 0.5 : -0.5)),
                    (FT_Pos)(y + (y >= 0 ? 0.5 : -0.5)));
    }
    c->px = to->x;
    c->py = to->y;
    return 0;
}

/* Flatten a cubic into short line segments (for CFF/OTF fonts). */
static int cb_cubic(const FT_Vector *c1, const FT_Vector *c2, const FT_Vector *to,
                    void *ud)
{
    contour_t *c = (contour_t *)ud;
    const int N = 12;
    FT_Pos x0, y0;
    int i;

    if (!c->open)
        contour_begin(c, to->x, to->y);
    x0 = c->px;
    y0 = c->py;
    for (i = 1; i <= N; i++) {
        double t = (double)i / N, u = 1.0 - t;
        double x = u * u * u * x0 + 3.0 * u * u * t * c1->x +
                   3.0 * u * t * t * c2->x + t * t * t * to->x;
        double y = u * u * u * y0 + 3.0 * u * u * t * c1->y +
                   3.0 * u * t * t * c2->y + t * t * t * to->y;
        line_add(c, (FT_Pos)(x + (x >= 0 ? 0.5 : -0.5)),
                    (FT_Pos)(y + (y >= 0 ? 0.5 : -0.5)));
    }
    c->px = to->x;
    c->py = to->y;
    return 0;
}

int font_glyph_outline(uint32_t handle, uint32_t ch, uint32_t format,
                       uint32_t gm, uint32_t cb, uint32_t buf,
                       uint32_t *out_size)
{
    FT_Face f;
    FT_UInt gi;
    FT_GlyphSlot s;
    obuf_t b;
    contour_t c;
    FT_Outline_Funcs funcs;
    uint32_t size;
    int ret = 0;

    FONT_LOCK();
    f = current_face(handle);
    if (!f || !out_size || format != 2u)     /* GGO_NATIVE only */
        goto done;
    gi = FT_Get_Char_Index(f, ch);
    if (FT_Load_Glyph(f, gi, FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING) != 0)
        goto done;
    s = f->glyph;

    if (gm) {
        M16(gm + 0)  = (uint16_t)((s->metrics.width + 63) >> 6);
        M16(gm + 2)  = (uint16_t)((s->metrics.height + 63) >> 6);
        M32(gm + 4)  = (uint32_t)(int32_t)(s->metrics.horiBearingX >> 6);
        M32(gm + 8)  = (uint32_t)(int32_t)(s->metrics.horiBearingY >> 6);
        M16(gm + 12) = (uint16_t)((s->advance.x + 32) >> 6);
        M16(gm + 14) = 0;
        /* CTexFont reads the advance from the padding word at +0x10. */
        M16(gm + 16) = M16(gm + 12);
        M16(gm + 18) = 0;
    }

    memset(&b, 0, sizeof(b));
    memset(&c, 0, sizeof(c));
    c.b = &b;

    if (s->format == FT_GLYPH_FORMAT_OUTLINE) {
        memset(&funcs, 0, sizeof(funcs));
        funcs.move_to = cb_move;
        funcs.line_to = cb_line;
        funcs.conic_to = cb_conic;
        funcs.cubic_to = cb_cubic;
        FT_Outline_Decompose(&s->outline, &funcs, &c);
        contour_finish(&c);
    }
    if (b.oom) {
        free(b.p);
        goto done;
    }
    size = (uint32_t)b.n;
    *out_size = size;
    if (cb && buf) {
        if (cb < size) {
            free(b.p);
            ret = 2;                /* caller's buffer too small */
            goto done;
        }
        if (size)
            memcpy(MEM + buf, b.p, size);
    }
    free(b.p);
    ret = 1;
done:
    FONT_UNLOCK();
    return ret;
}

/* -------------------------------------------------------------- bitmaps */
/* GDI renders anti-aliased glyphs with GetGlyphOutlineA:
 *
 *   GGO_BITMAP (1)       1 bpp, MSB first, stride ((w+31)/32)*4
 *   GGO_GRAY8_BITMAP (6) 8 bpp coverage, stride (w+3)&~3, values 0..64
 *
 * CTexFont asks for GGO_GRAY8 when its antialias flag is set (0x409B61 picks
 * fmt 6/1 from CTexFont+0x38) and then indexes its 256-entry palette
 * (CTexFont+0x58) with every source byte.  That palette only maps 0..64
 * (0,3,63,...251,255,255), so the FreeType 0..255 coverage must be scaled to
 * GDI's 0..64 range; feeding 0..255 made most pixels hit palette entries that
 * are 0 and produced the "noise" atlas.
 */
int font_glyph_bitmap(uint32_t handle, uint32_t ch, uint32_t format,
                      uint32_t gm, uint32_t cb, uint32_t buf,
                      uint32_t *out_size)
{
    FT_Face f;
    FT_UInt gi;
    FT_GlyphSlot s;
    FT_Bitmap *bm;
    int w, h, stride, size, adv, ret = 0;

    FONT_LOCK();
    f = current_face(handle);
    if (!f || !out_size || (format != 1u && format != 6u))
        goto done;
    gi = FT_Get_Char_Index(f, ch);
    if (FT_Load_Glyph(f, gi, FT_LOAD_RENDER | FT_LOAD_NO_BITMAP |
                               FT_LOAD_TARGET_NORMAL) != 0)
        goto done;
    s = f->glyph;
    bm = &s->bitmap;
    w = (int)bm->width;
    h = (int)bm->rows;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    stride = (format == 6u) ? ((w + 3) & ~3) : (((w + 31) / 32) * 4);
    size = stride * h;
    adv = (int)((s->advance.x + 32) >> 6);

    if (gm) {
        M16(gm + 0)  = (uint16_t)w;                  /* gmBlackBoxX */
        M16(gm + 2)  = (uint16_t)h;                  /* gmBlackBoxY */
        M32(gm + 4)  = (uint32_t)(int32_t)s->bitmap_left; /* gmptGlyphOrigin.x */
        M32(gm + 8)  = (uint32_t)(int32_t)s->bitmap_top;  /* +y upwards */
        M16(gm + 12) = (uint16_t)adv;                /* gmCellIncX */
        M16(gm + 14) = 0;
        M16(gm + 16) = (uint16_t)adv;                /* read at 0x409F5E */
        M16(gm + 18) = 0;
    }

    *out_size = (uint32_t)size;
    if (cb && buf) {
        if (cb < (uint32_t)size) {
            ret = 2;
            goto done;
        }
        memset(MEM + buf, 0, (size_t)size);
        for (int y = 0; y < h; y++) {
            const uint8_t *src = bm->buffer + (size_t)y * bm->pitch;
            uint8_t *dst = MEM + buf + (size_t)y * stride;
            for (int x = 0; x < w; x++) {
                unsigned cov;
                switch (bm->pixel_mode) {
                case FT_PIXEL_MODE_MONO:
                    cov = (src[x >> 3] & (0x80u >> (x & 7))) ? 255u : 0u;
                    break;
                case FT_PIXEL_MODE_GRAY2:
                    cov = ((src[x >> 2] >> (6 - 2 * (x & 3))) & 3u) * 85u;
                    break;
                case FT_PIXEL_MODE_GRAY4:
                    cov = ((src[x >> 1] >> (4 - 4 * (x & 1))) & 15u) * 17u;
                    break;
                case FT_PIXEL_MODE_BGRA:
                    cov = ((unsigned)src[x * 4 + 3] + src[x * 4 + 2] +
                           src[x * 4 + 1]) / 3u;
                    break;
                default:                        /* FT_PIXEL_MODE_GRAY */
                    cov = src[x];
                    break;
                }
                if (format == 6u)
                    dst[x] = (uint8_t)((cov * 64u + 127u) / 255u);
                else if (cov >= 128u)
                    dst[x >> 3] |= (uint8_t)(0x80u >> (x & 7));
            }
        }
    }
    ret = 1;
done:
    FONT_UNLOCK();
    return ret;
}
