/* platform_sdl.c - SDL3 window, keyboard/gamepad input and presentation.
 *
 * The window is created on first use.  The picture is composed on a 32-bit
 * ARGB software surface that the renderer draws into and that is uploaded to
 * a streaming texture once per frame, so the game works the same on Wayland,
 * X11/XWayland and the dummy driver used by the test harness.
 */

#define _GNU_SOURCE
#include "win32.h"
#include "platform.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_Window *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture *g_tex;
static int g_ready;
static int g_w = 1024, g_h = 768;
static uint32_t g_clear = 0xFF000000u;
static SDL_Gamepad *g_pad;
static int g_quit;

/* The software surface the renderer draws into. */
uint8_t *g_fb;
int g_fb_w, g_fb_h;

void platform_set_size(int w, int h)
{
    if (w > 0 && h > 0) {
        g_w = w;
        g_h = h;
    }
}

int platform_init(void)
{
    if (g_ready)
        return 0;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "platform: SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }
    g_win = SDL_CreateWindow("Otomedius", g_w, g_h,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!g_win) {
        fprintf(stderr, "platform: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return -1;
    }
    g_ren = SDL_CreateRenderer(g_win, NULL);
    if (!g_ren) {
        fprintf(stderr, "platform: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_win);
        SDL_Quit();
        return -1;
    }
    SDL_SetRenderVSync(g_ren, 1);
    g_fb = (uint8_t *)calloc((size_t)g_w * g_h * 4, 1);
    g_fb_w = g_w;
    g_fb_h = g_h;
    g_ready = 1;
    log_printf("platform: %s renderer", SDL_GetRendererName(g_ren));
    return 0;
}

void platform_shutdown(void)
{
    if (!g_ready)
        return;
    free(g_fb);
    g_fb = NULL;
    if (g_tex)
        SDL_DestroyTexture(g_tex);
    if (g_ren)
        SDL_DestroyRenderer(g_ren);
    if (g_win)
        SDL_DestroyWindow(g_win);
    SDL_Quit();
    g_ready = 0;
}

void platform_clear(uint32_t flags, uint32_t color)
{
    if (flags & 1)
        g_clear = 0xFF000000u | (color & 0x00FFFFFFu);
}

void platform_present(void)
{
    if (!g_ready)
        return;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT)
            win_request_quit();
        else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE)
            win_request_quit();
        else if (ev.type == SDL_EVENT_GAMEPAD_ADDED && !g_pad)
            g_pad = SDL_OpenGamepad(ev.gdevice.which);
    }
    SDL_SetRenderDrawColor(g_ren, (g_clear >> 16) & 0xFF, (g_clear >> 8) & 0xFF,
                           g_clear & 0xFF, 255);
    SDL_RenderClear(g_ren);
    SDL_RenderPresent(g_ren);
}

void platform_wait(void)
{
    if (g_ready)
        SDL_WaitEventTimeout(NULL, 1);
}

void platform_viewport(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
void platform_transform(uint32_t state, const void *matrix) { (void)state; (void)matrix; }
void platform_state(uint32_t state, uint32_t value) { (void)state; (void)value; }
void platform_sampler(uint32_t stage, uint32_t type, uint32_t value)
{
    (void)stage; (void)type; (void)value;
}
void platform_texture(uint32_t stage, uint32_t tex) { (void)stage; (void)tex; }
void platform_stream(uint32_t stream, uint32_t vb, uint32_t offset, uint32_t stride)
{
    (void)stream; (void)vb; (void)offset; (void)stride;
}
void platform_indices(uint32_t ib, uint32_t base) { (void)ib; (void)base; }
void platform_pixel_shader(uint32_t ps) { (void)ps; }
void platform_draw(uint32_t prim, uint32_t base_vertex, uint32_t min_index,
                   uint32_t num_vertices, uint32_t start_index, int indexed,
                   uint32_t prim_count)
{
    (void)prim; (void)base_vertex; (void)min_index; (void)num_vertices;
    (void)start_index; (void)indexed; (void)prim_count;
}
void platform_draw_up(uint32_t prim, uint32_t count, uint32_t data, uint32_t stride,
                      uint32_t fvf)
{
    (void)prim; (void)count; (void)data; (void)stride; (void)fvf;
}

/* ---------------------------------------------------------------- input */
static int key_down(SDL_Keycode k)
{
    const bool *state = SDL_GetKeyboardState(NULL);
    return state ? state[SDL_GetScancodeFromKey(k, NULL)] : 0;
}

void platform_read_pad(pad_state_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!g_ready)
        return;
    if (key_down(SDLK_UP) || key_down(SDLK_W))
        out->up = 1;
    if (key_down(SDLK_DOWN) || key_down(SDLK_S))
        out->down = 1;
    if (key_down(SDLK_LEFT) || key_down(SDLK_A))
        out->left = 1;
    if (key_down(SDLK_RIGHT) || key_down(SDLK_D))
        out->right = 1;
    if (key_down(SDLK_Z) || key_down(SDLK_J))
        out->b1 = 1;
    if (key_down(SDLK_X) || key_down(SDLK_K))
        out->b2 = 1;
    if (key_down(SDLK_C) || key_down(SDLK_L))
        out->b3 = 1;
    if (key_down(SDLK_V) || key_down(SDLK_I))
        out->b4 = 1;
    if (key_down(SDLK_RETURN) || key_down(SDLK_KP_ENTER))
        out->start = 1;
    if (key_down(SDLK_5) || key_down(SDLK_6))
        out->coin = 1;
    if (g_pad) {
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_DPAD_UP) ||
            SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTY) < -8000)
            out->up = 1;
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN) ||
            SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTY) > 8000)
            out->down = 1;
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT) ||
            SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTX) < -8000)
            out->left = 1;
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ||
            SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTX) > 8000)
            out->right = 1;
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_SOUTH))
            out->b1 = 1;
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_EAST))
            out->b2 = 1;
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_WEST))
            out->b3 = 1;
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_NORTH))
            out->b4 = 1;
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_START))
            out->start = 1;
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_BACK))
            out->coin = 1;
    }
}
