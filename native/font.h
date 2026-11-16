/* font.h - FreeType-backed GDI font backend.
 *
 * The game builds its font atlases with GDI (CreateFontA / SelectObject /
 * GetTextMetricsA / GetCharABCWidthsA / GetGlyphOutlineA GGO_NATIVE) and
 * rasterises the returned outlines itself.  The original implementation
 * synthesised a rectangle for every glyph; this backend returns the real
 * outlines and metrics from a system TrueType/OpenType font instead.
 *
 * All functions return 0 when FreeType is unavailable or the glyph cannot be
 * produced, so the caller can fall back to the historical synthetic path.
 */
#ifndef FONT_H
#define FONT_H

#include <stdint.h>

void     font_init(void);
void     font_shutdown(void);
int      font_available(void);

/* Register a font file that the game supplies in memory (AddFontMemResourceEx).
 * The blob lives in emulated MEM for the rest of the run, so FreeType can keep
 * a memory face backed by it.  Returns 1 when the blob is a usable font. */
int      font_add_memory(const void *data, unsigned long size);

/* Create/select/delete a GDI font object.  `handle` is the value returned by
 * CreateFontA and must be passed to SelectObject/DeleteObject. */
uint32_t font_create(int height, const char *face);
void     font_select(uint32_t handle);
void     font_destroy(uint32_t handle);

/* Write TEXTMETRICA (56 bytes) or ABC widths into emulated memory. */
int font_text_metrics(uint32_t handle, uint32_t tm);
int font_abc_widths(uint32_t handle, uint32_t first, uint32_t last, uint32_t abc);

/* GGO_NATIVE outline.  Writes GLYPHMETRICS into `gm`, sets `*out_size` to the
 * size of the TTPOLYGONHEADER/TTPOLYCURVE stream and, when `cb != 0 &&
 * buf != 0`, copies it into emulated memory at `buf`.
 *
 * Returns 1 on success, 2 when the caller's buffer is too small, and 0 when
 * this backend cannot handle the request (caller falls back). */
int font_glyph_outline(uint32_t handle, uint32_t ch, uint32_t format,
                       uint32_t gm, uint32_t cb, uint32_t buf,
                       uint32_t *out_size);

/* GGO_BITMAP (1) / GGO_GRAY8_BITMAP (6).  Renders the glyph with FreeType and
 * returns the same buffer conventions as GetGlyphOutlineA: GGO_BITMAP is 1bpp
 * MSB-first with stride ((w+31)/32)*4, GGO_GRAY8 is one byte per pixel with
 * stride (w+3)&~3, in GDI's 0..64 range (the game maps it through its palette
 * at CTexFont+0x58).  Returns 1 on success, 2 when the caller's buffer is too
 * small, 0 when unavailable. */
int font_glyph_bitmap(uint32_t handle, uint32_t ch, uint32_t format,
                      uint32_t gm, uint32_t cb, uint32_t buf,
                      uint32_t *out_size);

#endif /* FONT_H */
