/* win32_misc.c - gdi32, ole32, oleaut32, shlwapi, imm32, winmm, dsound. */

#define _GNU_SOURCE
#include "win32.h"
#include "font.h"

#include <string.h>
#include <stdlib.h>
#include <iconv.h>

static void misc_stub(void) { RET32(0); }

/* Log helper: the game passes Shift-JIS face names, which look like garbage in
 * a UTF-8 terminal.  Convert CP932 -> UTF-8 for the message, fall back to a
 * plain marker when iconv cannot do it. */
const char *win_sjis_to_utf8(const char *src, char *dst, size_t cap)
{
    iconv_t cd = iconv_open("UTF-8", "CP932");
    char *in = (char *)src;
    size_t inleft = strlen(src), outleft = cap - 1;
    char *out = dst;

    if (cd == (iconv_t)-1) {
        snprintf(dst, cap, "%.*s", (int)(cap - 1), src);
        return dst;
    }
    if (iconv(cd, &in, &inleft, &out, &outleft) == (size_t)-1) {
        iconv_close(cd);
        snprintf(dst, cap, "%.*s", (int)(cap - 1), src);
        return dst;
    }
    iconv_close(cd);
    *out = 0;
    return dst;
}

/* -------------------------------------------------------------- gdi32
 * The game builds its font atlases itself: CreateFontA + SelectObject, then
 * GetTextMetricsA / GetCharABCWidthsA for the layout and GetGlyphOutlineA
 * (GGO_BITMAP) per glyph.  GetGlyphOutlineA returning GDI_ERROR aborts the
 * atlas build and the game logs 1117 CTexFont::GetFontBuffer and suspends
 * every manager, so these have to answer coherently.  Glyphs are synthesised
 * as boxes (there is no font file behind the DC yet); the atlas, metrics and
 * text layout are the real thing. */
static int g_font_h = 24;

static void g_CreateFontA(void)
{
    int h = (int)arg32(0);
    uint32_t fh;
    g_font_h = h < 0 ? -h : (h ? h : 16);
    if (g_font_h < 6)
        g_font_h = 6;
    if (g_font_h > 128)
        g_font_h = 128;
    {
        char face[128];
        const char *f = arg_str(13);
        log_printf("gdi32: CreateFontA(h=%d face=%s)", h,
                   f ? win_sjis_to_utf8(f, face, sizeof(face)) : "?");
    }
    fh = font_create(g_font_h, arg_str(13));
    RET32(fh ? fh : 0x0F000001u);
}
static void g_DeleteObject(void)
{
    font_destroy(arg32(0));
    RET32(1);
}
static void g_SelectObject(void)
{
    font_select(arg32(1));
    RET32(0x0F000002u);
}

#define GGO_BITMAP 1u
#define GGO_NATIVE 2u

/* One closed rectangle contour, in GDI's native outline format:
 *   TTPOLYGONHEADER { DWORD cb; DWORD dwType = TT_POLYGON_TYPE; POINTFX pfxStart; }
 *   TTPOLYCURVE     { WORD wType = TT_PRIM_LINE; WORD cpfx; POINTFX apfx[]; }
 * Coordinates are 16.16 fixed point with +y upwards from the baseline, which
 * is what CTexFont::GetFontBuffer rasterises into the atlas. */
#define GLYPH_NATIVE_SIZE (16 + 4 + 4 * 8)

static void put_fixed(uint32_t p, float v)
{
    M32(p) = (uint32_t)(int32_t)(v * 65536.0f);
}

static void glyph_native_outline(uint32_t p, int w, int h)
{
    M32(p + 0) = GLYPH_NATIVE_SIZE;      /* cb */
    M32(p + 4) = 24;                     /* TT_POLYGON_TYPE */
    put_fixed(p + 8, 1.0f);              /* pfxStart.x */
    put_fixed(p + 12, (float)h);         /* pfxStart.y */
    M16(p + 16) = 1;                     /* TT_PRIM_LINE */
    M16(p + 18) = 4;                     /* cpfx */
    put_fixed(p + 20, (float)w); put_fixed(p + 24, (float)h);
    put_fixed(p + 28, (float)w); put_fixed(p + 32, 0.0f);
    put_fixed(p + 36, 1.0f);     put_fixed(p + 40, 0.0f);
    put_fixed(p + 44, 1.0f);     put_fixed(p + 48, (float)h);
}

/* The game calls the GDI import with an extra (7th) argument: a second
 * GLYPHMETRICS pointer embedded in the CTexFont object at +0x3A.  The Win32
 * API itself only has six arguments (the local gm at arg3 is the API one),
 * but CTexFont reads the one at arg6 to size the glyph in its atlas.  Leaving
 * it untouched made the atlas builder see garbage dimensions, which packed
 * glyphs over each other and eventually corrupted the heap. */
static void gm_mirror(uint32_t gm, uint32_t gm2)
{
    /* The 7th argument is a second GLYPHMETRICS-shaped buffer inside the
     * CTexFont object.  It is tempting to mirror the API's gm there, but the
     * font code pre-fills those fields with its own UV/scale values; leave
     * them alone until the exact contract is known. */
    (void)gm;
    (void)gm2;
}

static void g_GetGlyphOutlineA(void)
{
    uint32_t ch = arg32(1), format = arg32(2), gm = arg32(3);
    uint32_t cb = arg32(4), buf = arg32(5), gm2 = arg32(6);
    int h = g_font_h, w = (g_font_h * 2) / 3;
    int stride, size;

    {
        static int n;
        if (g_verbose && n++ < 10)
            fprintf(stderr, "gdi32: GetGlyphOutlineA(ch=%u fmt=%u gm=%08x "
                    "gm2=%08x cb=%u buf=%08x)\n",
                    ch, format, gm, gm2, cb, buf);
    }
    if (w < 2)
        w = 2;

    if (format & GGO_NATIVE) {
        /* Prefer the real font: the game rasterises these outlines into its
         * atlas, so a genuine glyph shape is what makes text readable. */
        uint32_t fsize = 0;
        int fr = font_glyph_outline(0, ch, format, gm, cb, buf, &fsize);
        if (fr == 1) {
            gm_mirror(gm, gm2);
            RET32(fsize);
            return;
        }
        if (fr == 2) {
            gm_mirror(gm, gm2);
            RET32(0xFFFFFFFFu);          /* GDI_ERROR: buffer too small */
            return;
        }
        /* no FreeType/font: fall back to the synthetic rectangle */
        size = GLYPH_NATIVE_SIZE;
        if (gm) {
            M16(gm + 0) = (uint16_t)w;          /* gmBlackBoxX */
            M16(gm + 2) = (uint16_t)h;          /* gmBlackBoxY */
            M32(gm + 4) = 1;                    /* gmptGlyphOrigin.x */
            M32(gm + 8) = 0;                    /* gmptGlyphOrigin.y */
            M16(gm + 12) = (uint16_t)(w + 1);   /* gmCellIncX */
            M16(gm + 14) = 0;
        }
        gm_mirror(gm, gm2);
        if (!cb || !buf)
            RET32((uint32_t)size);
        else if (cb < (uint32_t)size)
            RET32(0xFFFFFFFFu);
        else {
            glyph_native_outline(buf, w, h);
            RET32((uint32_t)size);
        }
        return;
    }

    stride = ((w + 31) / 32) * 4;
    size = stride * h;

    if (gm) {
        M16(gm + 0) = (uint16_t)w;          /* gmBlackBoxX */
        M16(gm + 2) = (uint16_t)h;          /* gmBlackBoxY */
        M32(gm + 4) = 1;                    /* gmptGlyphOrigin.x */
        M32(gm + 8) = (uint32_t)(h - 2);    /* gmptGlyphOrigin.y (baseline) */
        M16(gm + 12) = (uint16_t)(w + 1);   /* gmCellIncX */
        M16(gm + 14) = 0;
    }
    gm_mirror(gm, gm2);
    if (format != GGO_BITMAP) {
        RET32(0xFFFFFFFFu);                 /* GDI_ERROR */
        return;
    }
    if (!cb || !buf) {                      /* size query */
        RET32((uint32_t)size);
        return;
    }
    if (cb < (uint32_t)size) {
        RET32(0xFFFFFFFFu);
        return;
    }
    memset(MEM + buf, 0, (size_t)size);
    /* 1bpp, MSB first: a box outline so the layout is readable */
    for (int y = 0; y < h; y++) {
        uint8_t *row = MEM + buf + (size_t)y * stride;
        row[0] |= 0x80;
        row[(w - 1) >> 3] |= (uint8_t)(0x80 >> ((w - 1) & 7));
    }
    for (int x = 0; x < w; x++) {
        MEM[buf + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
        MEM[buf + (size_t)(h - 1) * stride + (x >> 3)] |=
            (uint8_t)(0x80 >> (x & 7));
    }
    if (g_verbose > 1) {
        static int n;
        if (n++ < 6)
            fprintf(stderr, "gdi32: GetGlyphOutline(ch %u) %dx%d -> %d bytes\n",
                    ch, w, h, size);
    }
    RET32((uint32_t)size);
}

static void g_GetTextMetricsA(void)
{
    uint32_t tm = arg32(1);
    int h = g_font_h;
    if (tm && font_text_metrics(0, tm)) {
        RET32(1);
        return;
    }
    if (tm) {
        /* sizeof(TEXTMETRICA) == 56 on Win32.  Do NOT memset 64: the game's
         * CTexFont passes a 0x3C-byte local at ebp-0x3C, so a 64-byte clear
         * writes over the saved EBP and the caller returns with ebp = 0. */
        memset(MEM + tm, 0, 56);
        M32(tm + 0) = (uint32_t)h;               /* tmHeight */
        M32(tm + 4) = (uint32_t)(h - h / 4);     /* tmAscent */
        M32(tm + 8) = (uint32_t)(h / 4);         /* tmDescent */
        M32(tm + 12) = (uint32_t)(h / 8);        /* tmInternalLeading */
        M32(tm + 20) = (uint32_t)((h * 2) / 3);  /* tmAveCharWidth */
        M32(tm + 24) = (uint32_t)h;              /* tmMaxCharWidth */
        M32(tm + 28) = 400;                      /* tmWeight */
        M8(tm + 44) = 0x20;                      /* tmFirstChar */
        M8(tm + 45) = 0xFF;                      /* tmLastChar */
        M8(tm + 46) = 0x3F;                      /* tmDefaultChar */
        M8(tm + 47) = 0x20;                      /* tmBreakChar */
        M8(tm + 52) = 0;                         /* tmCharSet (ANSI) */
    }
    RET32(1);
}

static void g_GetCharABCWidthsA(void)
{
    uint32_t first = arg32(1), last = arg32(2), abc = arg32(3);
    int adv = (g_font_h * 2) / 3 + 1;
    if (font_abc_widths(0, first, last, abc)) {
        RET32(1);
        return;
    }
    if (abc && last >= first && last - first < 0x10000) {
        for (uint32_t c = first; c <= last; c++) {
            uint32_t p = abc + (c - first) * 6;
            M16(p + 0) = 0;                     /* a: left side bearing */
            M16(p + 2) = (uint16_t)adv;         /* b: black width */
            M16(p + 4) = 0;                     /* c: right side bearing */
        }
    }
    RET32(1);
}
static void g_AddFontMemResourceEx(void) { RET32(0); }

const import_def_t gdi32_imports[] = {
    {"gdi32.dll", "CreateFontA", g_CreateFontA, 14},
    {"gdi32.dll", "DeleteObject", g_DeleteObject, 1},
    {"gdi32.dll", "GetGlyphOutlineA", g_GetGlyphOutlineA, 7},
    {"gdi32.dll", "GetTextMetricsA", g_GetTextMetricsA, 2},
    {"gdi32.dll", "GetCharABCWidthsA", g_GetCharABCWidthsA, 4},
    {"gdi32.dll", "AddFontMemResourceEx", g_AddFontMemResourceEx, 4},
    {"gdi32.dll", "SelectObject", g_SelectObject, 2},
    {NULL, NULL, NULL, 0},
};

/* ------------------------------------------------------------- ole32 */
static void o_CoTaskMemFree(void) { }
static void o_CoCreateInstance(void)
{
    uint32_t rclsid = arg32(0), riid = arg32(3), ppv = arg32(4);
    int r = dshow_create_filter_graph(rclsid, riid, ppv);
    if (r == 1) { RET32(0); return; }              /* S_OK */
    if (r == 2) { RET32(0x80004002u); return; }    /* E_NOINTERFACE */
    if (rclsid) {
        const uint8_t *c = MEM + rclsid;
        log_printf("ole32: CoCreateInstance {%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x} "
                   "riid {%08x-%04x-...} -> not implemented",
                   M32(rclsid), M16(rclsid + 4), M16(rclsid + 6), c[8], c[9], c[10],
                   c[11], c[12], c[13], c[14], c[15],
                   riid ? M32(riid) : 0, riid ? M16(riid + 4) : 0);
    }
    if (ppv)
        M32(ppv) = 0;
    RET32(0x80040154u);   /* REGDB_E_CLASSNOTREG */
}
static void o_CoInitialize(void) { RET32(0); }
static void o_CoFreeUnusedLibraries(void) { }
static void o_CoTaskMemAlloc(void)
{
    /* DirectShow copies every accepted media type with CoTaskMemAlloc
     * (0x52DA30 in the game's filter).  Returning NULL here made the movie
     * renderer fail with E_OUTOFMEMORY before it ever created its texture. */
    extern uint32_t emu_alloc(uint32_t size);
    uint32_t size = arg32(0);
    if (!size)
        size = 1;
    RET32(emu_alloc(size));
}
static void o_CoUninitialize(void) { }
static void o_SysAllocString(void) { RET32(0); }
static void o_SysFreeString(void) { }

const import_def_t ole_imports[] = {
    {"ole32.dll", "CoTaskMemFree", o_CoTaskMemFree, 1},
    {"ole32.dll", "CoCreateInstance", o_CoCreateInstance, 5},
    {"ole32.dll", "CoInitialize", o_CoInitialize, 1},
    {"ole32.dll", "CoFreeUnusedLibraries", o_CoFreeUnusedLibraries, 0},
    {"ole32.dll", "CoTaskMemAlloc", o_CoTaskMemAlloc, 1},
    {"ole32.dll", "CoUninitialize", o_CoUninitialize, 0},
    {"oleaut32.dll", "ordinal:2", o_SysAllocString, 1},
    {"oleaut32.dll", "ordinal:6", o_SysFreeString, 1},
    {"shlwapi.dll", "PathRelativePathToA", misc_stub, 6},
    {NULL, NULL, NULL, 0},
};

/* ------------------------------------------------------------- winmm */
static void w_timeKillEvent(void) { RET32(0); }
static void w_timeSetEvent(void) { RET32(1); }
static void w_timeBeginPeriod(void) { RET32(0); }
static void w_timeEndPeriod(void) { RET32(0); }
static void w_timeGetTime(void)
{
    extern void win_tick(uint32_t *lo);
    uint32_t v = 0;
    win_tick(&v);
    RET32(v);
}

const import_def_t winmm_imports[] = {
    {"winmm.dll", "timeKillEvent", w_timeKillEvent, 1},
    {"winmm.dll", "timeSetEvent", w_timeSetEvent, 5},
    {"winmm.dll", "timeBeginPeriod", w_timeBeginPeriod, 1},
    {"winmm.dll", "timeGetTime", w_timeGetTime, 0},
    {"winmm.dll", "timeEndPeriod", w_timeEndPeriod, 1},
    {NULL, NULL, NULL, 0},
};

const import_def_t imm32_imports[] = {
    {"imm32.dll", "ImmDisableIME", misc_stub, 1},
    {NULL, NULL, NULL, 0},
};

