/* platform.h - the window/GPU/input backend (SDL3). */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>

int  platform_init(void);
void platform_shutdown(void);
void platform_present(void);
void platform_clear(uint32_t flags, uint32_t color);
void platform_viewport(int x, int y, int w, int h);
void platform_transform(uint32_t state, const void *matrix);
void platform_state(uint32_t state, uint32_t value);
void platform_sampler(uint32_t stage, uint32_t type, uint32_t value);
void platform_texture(uint32_t stage, uint32_t tex);
void platform_stream(uint32_t stream, uint32_t vb, uint32_t offset, uint32_t stride);
void platform_indices(uint32_t ib, uint32_t base);
void platform_pixel_shader(uint32_t ps);
void platform_draw(uint32_t prim, uint32_t base_vertex, uint32_t min_index,
                   uint32_t num_vertices, uint32_t start_index, int indexed,
                   uint32_t prim_count);
void platform_draw_up(uint32_t prim, uint32_t count, uint32_t data, uint32_t stride,
                      uint32_t fvf);
void platform_set_size(int w, int h);
void platform_wait(void);

/* Audio output for the opening movie (SDL backend; the headless backend
 * accepts the calls and discards the samples). */
int  platform_audio_open(int rate, int channels);
void platform_audio_write(const void *data, int len);
void platform_audio_close(void);

/* keyboard state as arcade cabinet buttons */
typedef struct {
    int coin, start, up, down, left, right;
    int b1, b2, b3, b4;
} pad_state_t;

void platform_read_pad(pad_state_t *out);

/* Touch panel (the cabinet had one; Otomedius drove it with a pen).  Devices
 * that report SDL pen events (Wacom and friends) take priority; a normal mouse
 * works as a fallback so the panel is usable without a tablet.  Coordinates are
 * normalised 0..1 over the window, so the AVS layer can map them to whatever
 * space the game expects. */
typedef struct {
    float x, y;      /* 0..1 over the game window */
    int pressed;     /* pen/mouse is touching the panel */
    int pressure;    /* 0..255, 0 when unknown */
} touch_state_t;

void platform_read_touch(touch_state_t *out);

/* the software frame buffer produced by render.c (0xAARRGGBB, w*h) */
const uint8_t *render_framebuffer(int *w, int *h);

#endif
