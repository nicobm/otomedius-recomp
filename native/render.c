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

static int g_width = 1024, g_height = 768;
static uint32_t g_clear = 0xFF000000u;

int render_width(void) { return g_width; }
int render_height(void) { return g_height; }

void render_init(int w, int h)
{
    if (w > 0 && h > 0) {
        g_width = (int)w;
        g_height = (int)h;
    }
    platform_set_size(g_width, g_height);
}

void render_present(void)
{
    platform_present();
}

void render_clear(int count, uint32_t rects, uint32_t flags, uint32_t color)
{
    (void)count;
    (void)rects;
    if (flags & 1)
        g_clear = color & 0x00FFFFFFu;
    platform_clear(flags, color);
}

void render_set_viewport(int x, int y, int w, int h)
{
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
    platform_texture(stage, tex);
}

void render_set_stream(uint32_t stream, uint32_t vb, uint32_t offset, uint32_t stride)
{
    platform_stream(stream, vb, offset, stride);
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
    platform_draw(prim, base_vertex, min_index, num_vertices, start_index,
                  indexed, prim_count);
}

void render_draw_up(uint32_t prim, uint32_t count, uint32_t data, uint32_t stride,
                    uint32_t fvf)
{
    platform_draw_up(prim, count, data, stride, fvf);
}
