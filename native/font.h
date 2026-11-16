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

#endif /* FONT_H */
