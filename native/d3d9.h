/* d3d9.h - shared declarations of the Direct3D 9 layer. */
#ifndef D3D9_H
#define D3D9_H

#include <stdint.h>

typedef struct {
    void (*fn)(void);
    int nargs;
} method_t;

uint32_t emu_alloc(uint32_t size);
uint32_t com_create(const method_t *methods, int count, uint32_t extra);
uint32_t com_create_named(const char *name, const method_t *methods, int count,
                          uint32_t extra);
/* "Interface::Method" for a call through a vtable we created, or NULL */
const char *com_method_name(uint32_t thunk);
uint32_t com_vtable(uint32_t obj);
uint32_t com_refcount(uint32_t obj);

uint32_t device_create(uint32_t w, uint32_t h);
uint32_t texture_create(uint32_t w, uint32_t h, uint32_t levels, uint32_t usage,
                        uint32_t format, uint32_t pool);
uint32_t buffer_create(uint32_t size, uint32_t usage, uint32_t fvf, int is_index);
/* Read-only view of a vertex/index buffer for the software rasteriser.
 * Returns the emulated pointer to the data, or NULL for an invalid object. */
const void *buffer_data(uint32_t obj, uint32_t *size, int *is_index,
                        uint32_t *format);
uint32_t surface_create(uint32_t w, uint32_t h, uint32_t format, int renderable);
uint32_t shader_create(uint32_t code_addr, int pixel);

/* render.c */
int  render_width(void);
int  render_height(void);
void render_init(int w, int h);
void render_present(void);
void render_clear(int count, uint32_t rects, uint32_t flags, uint32_t color);
void render_set_viewport(int x, int y, int w, int h);
void render_set_transform(uint32_t state, const void *matrix);
void render_set_state(uint32_t state, uint32_t value);
void render_set_sampler(uint32_t stage, uint32_t type, uint32_t value);
void render_set_texture(uint32_t stage, uint32_t tex);
void render_set_stream(uint32_t stream, uint32_t vb, uint32_t offset, uint32_t stride);
void render_set_fvf(uint32_t fvf);
void render_set_indices(uint32_t ib, uint32_t base);
void render_set_pixel_shader(uint32_t ps);
void render_draw(uint32_t prim, uint32_t base_vertex, uint32_t min_index,
                 uint32_t num_vertices, uint32_t start_index, int indexed,
                 uint32_t prim_count);
void render_draw_ship(const float *world);
void render_draw_up(uint32_t prim, uint32_t count, uint32_t data, uint32_t stride,
                    uint32_t fvf);
uint32_t texture_data(uint32_t tex);
uint32_t texture_width(uint32_t tex);
uint32_t texture_height(uint32_t tex);
const uint32_t *texture_argb(uint32_t obj, uint32_t *w, uint32_t *h);
const uint8_t *render_framebuffer(int *w, int *h);

#endif
