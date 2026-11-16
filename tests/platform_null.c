/* Headless platform backend used by the local test harness: no window, no
 * GPU, just enough to let the game logic run. */
#include "win32.h"
#include "platform.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static int g_w = 640, g_h = 480;
int platform_init(void) { fprintf(stderr, "[platform] headless\n"); return 0; }
void platform_shutdown(void) {}
void platform_present(void) {}
void platform_clear(uint32_t flags, uint32_t color) { (void)flags; (void)color; }
void platform_viewport(int x, int y, int w, int h) { (void)x;(void)y;(void)w;(void)h; }
void platform_transform(uint32_t s, const void *m) { (void)s;(void)m; }
void platform_state(uint32_t s, uint32_t v) { (void)s;(void)v; }
void platform_sampler(uint32_t a, uint32_t b, uint32_t c) { (void)a;(void)b;(void)c; }
void platform_texture(uint32_t a, uint32_t b) { (void)a;(void)b; }
void platform_stream(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { (void)a;(void)b;(void)c;(void)d; }
void platform_indices(uint32_t a, uint32_t b) { (void)a;(void)b; }
void platform_pixel_shader(uint32_t a) { (void)a; }
void platform_draw(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, int f, uint32_t g) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g; }
void platform_draw_up(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) { (void)a;(void)b;(void)c;(void)d;(void)e; }
void platform_set_size(int w, int h) { g_w = w; g_h = h; }
void platform_wait(void) {}
void platform_read_pad(pad_state_t *o) { memset(o, 0, sizeof(*o)); }
uint32_t texture_data(uint32_t t) { return t; }
uint32_t texture_width(uint32_t t) { (void)t; return 0; }
uint32_t texture_height(uint32_t t) { (void)t; return 0; }
