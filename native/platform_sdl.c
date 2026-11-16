/* platform_sdl.c - SDL3 window, keyboard/gamepad input and presentation.
 *
 * The window is created on first use.  The picture is composed on a 32-bit
 * ARGB software surface that the renderer draws into and that is uploaded to
 * a streaming texture once per frame, so the game works the same on Wayland,
 * X11/XWayland and the dummy driver used by the test harness.
 *
 * Bezel: when the game directory ships a bezel.png (the arcade cabinet art,
 * 1920x1080 in this dump) the window is sized to the artwork, the game frame
 * is fitted into the transparent screen hole and the art is drawn on top.
 * That is how the cabinet sides show up when the game opens.  OTOMEDIUS_BEZEL
 * can point at another PNG or be set to 0/off to disable it.  The cabinet view
 * opens fullscreen; F11 toggles and OTOMEDIUS_WINDOWED=1 starts windowed
 * (OTOMEDIUS_FULLSCREEN=1 forces fullscreen without a bezel).
 */

#define _GNU_SOURCE
#include "win32.h"
#include "platform.h"
#include "movie.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static SDL_Window *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture *g_tex;
static int g_ready;
static int g_w = 1024, g_h = 768;
static uint32_t g_clear = 0xFF000000u;
static SDL_Gamepad *g_pad;
static int g_quit;

/* touch panel state, fed by pen (tablet) or mouse events */
static struct {
    float x, y;
    int pressed;
    int pressure;
    int pen_seen;      /* a real pen has been used: prefer it over the mouse */
} g_touch;

/* Size of the streaming texture fed by render.c's software frame buffer. */
static int g_fb_w, g_fb_h;

/* ---------------------------------------------------------------- bezel
 * The cabinet artwork and the rectangle the game picture occupies.  The hole
 * is detected from the PNG's alpha channel: the columns that are transparent
 * over (almost) the whole height, then the transparent rows inside them. */
static SDL_Texture *g_bezel;
static int g_bezel_w, g_bezel_h;
static SDL_FRect g_bezel_dst;      /* where the artwork goes, in window pixels */
static SDL_FRect g_game_dst;       /* where the game frame goes, in window pixels */
static struct { int x, y, w, h; } g_hole;   /* the screen hole, in bezel pixels */
static int g_bezel_overlay;        /* 1 = draw the art over the game frame */
static int g_fullscreen;
static unsigned g_present_count;
static float g_ev_scale_x = 1.0f, g_ev_scale_y = 1.0f;

/* Movie audio: a produced SDL audio stream fed by the decoder thread. */
static SDL_AudioStream *g_astream;
static int g_arate, g_achannels;

/* Load the cabinet art and find its transparent screen hole.  Returns 0 when
 * the file is missing or unreadable (the port then runs exactly as before). */
static SDL_Surface *bezel_open(const char *path)
{
    SDL_Surface *s = SDL_LoadPNG(path);
    SDL_Surface *argb;
    const uint8_t *px;
    int *col;
    int x, y, run = 0, best = 0, bx0 = 0, bx1 = -1, run_start = 0;
    int y0 = -1, y1 = -1, rrun = 0, rbest = 0, rrun_start = 0;
    int pitch;
    uint8_t alpha_center;

    if (!s) {
        log_printf("platform: no bezel (%s): %s", path, SDL_GetError());
        return NULL;
    }
    argb = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ARGB8888);
    SDL_DestroySurface(s);
    if (!argb) {
        log_printf("platform: bezel %s: convert failed: %s", path, SDL_GetError());
        return NULL;
    }
    if (argb->w < 8 || argb->h < 8) {
        log_printf("platform: bezel %s: %dx%d is too small",
                   path, argb->w, argb->h);
        SDL_DestroySurface(argb);
        return NULL;
    }

    /* Columns that are transparent from top to bottom are the screen hole.
     * The 1/64 slack tolerates a couple of opaque anti-aliasing pixels. */
    px = (const uint8_t *)argb->pixels;
    pitch = argb->pitch;
    col = (int *)calloc((size_t)argb->w, sizeof(int));
    if (!col) {
        SDL_DestroySurface(argb);
        return NULL;
    }
    for (x = 0; x < argb->w; x++) {
        int n = 0;
        for (y = 0; y < argb->h; y++)
            if (px[(size_t)y * pitch + x * 4 + 3] == 0)
                n++;
        col[x] = n;
    }
    for (x = 0; x < argb->w; x++) {
        if (col[x] * 64 >= argb->h * 63) {
            if (run++ == 0)
                run_start = x;
            if (run > best) {
                best = run;
                bx0 = run_start;
                bx1 = x;
            }
        } else {
            run = 0;
        }
    }
    free(col);

    /* Now the transparent rows inside the hole columns. */
    if (best > 0) {
        int span = bx1 - bx0 + 1;
        for (y = 0; y < argb->h; y++) {
            int n = 0;
            for (x = bx0; x <= bx1; x++)
                if (px[(size_t)y * pitch + x * 4 + 3] == 0)
                    n++;
            if (n * 64 >= span * 63) {
                if (rrun++ == 0)
                    rrun_start = y;
                if (rrun > rbest) {
                    rbest = rrun;
                    y0 = rrun_start;
                    y1 = y;
                }
            } else {
                rrun = 0;
            }
        }
    }

    if (best <= 0 || y1 < 0) {
        /* No transparent hole: draw the game over the whole window and put
         * the art underneath, so at least the sides stay visible. */
        g_hole.x = 0;
        g_hole.y = 0;
        g_hole.w = argb->w;
        g_hole.h = argb->h;
        g_bezel_overlay = 0;
        log_printf("platform: bezel %s %dx%d: no transparent screen hole",
                   path, argb->w, argb->h);
    } else {
        g_hole.x = bx0;
        g_hole.y = y0;
        g_hole.w = bx1 - bx0 + 1;
        g_hole.h = y1 - y0 + 1;
        alpha_center = px[(size_t)(y0 + g_hole.h / 2) * pitch +
                          (bx0 + g_hole.w / 2) * 4 + 3];
        g_bezel_overlay = alpha_center < 8;   /* transparent screen: draw on top */
    }
    g_bezel_w = argb->w;
    g_bezel_h = argb->h;
    log_printf("platform: bezel %s %dx%d hole %d,%d-%d,%d (%dx%d) %s",
               path, g_bezel_w, g_bezel_h, g_hole.x, g_hole.y,
               g_hole.x + g_hole.w - 1, g_hole.y + g_hole.h - 1,
               g_hole.w, g_hole.h,
               g_bezel_overlay ? "overlay" : "underlay");
    return argb;
}

/* Pick the artwork: OTOMEDIUS_BEZEL (a path, or 0/off to disable), then the
 * game directory, then the current directory. */
static const char *bezel_choose(void)
{
    const char *env = getenv("OTOMEDIUS_BEZEL");
    static char path[4096];

    if (env && (!strcmp(env, "0") || !strcmp(env, "off"))) {
        log_printf("platform: bezel disabled by OTOMEDIUS_BEZEL");
        return NULL;
    }
    if (env && *env) {
        if (access(env, R_OK) == 0)
            return env;
        log_printf("platform: OTOMEDIUS_BEZEL=%s is not readable", env);
    }
    snprintf(path, sizeof(path), "%s/bezel.png", win_game_dir());
    if (access(path, R_OK) == 0)
        return path;
    if (access("bezel.png", R_OK) == 0)
        return "bezel.png";
    return NULL;
}

/* Window layout: scale the artwork to fit while keeping its aspect, then fit
 * the game frame inside the hole the same way.  This keeps the cabinet art
 * intact when the window is resized or goes fullscreen. */
static void bezel_layout(int win_w, int win_h, int fw, int fh)
{
    SDL_FRect hole;
    float scale, gs;

    if (!g_bezel) {
        g_bezel_dst.x = g_bezel_dst.y = 0.0f;
        g_bezel_dst.w = (float)win_w;
        g_bezel_dst.h = (float)win_h;
        g_game_dst = g_bezel_dst;
        return;
    }
    scale = SDL_min((float)win_w / (float)g_bezel_w,
                    (float)win_h / (float)g_bezel_h);
    g_bezel_dst.w = g_bezel_w * scale;
    g_bezel_dst.h = g_bezel_h * scale;
    g_bezel_dst.x = ((float)win_w - g_bezel_dst.w) * 0.5f;
    g_bezel_dst.y = ((float)win_h - g_bezel_dst.h) * 0.5f;

    hole.x = g_bezel_dst.x + g_hole.x * scale;
    hole.y = g_bezel_dst.y + g_hole.y * scale;
    hole.w = g_hole.w * scale;
    hole.h = g_hole.h * scale;

    if (fw <= 0 || fh <= 0) {
        g_game_dst = hole;
        return;
    }
    gs = SDL_min(hole.w / (float)fw, hole.h / (float)fh);
    g_game_dst.w = fw * gs;
    g_game_dst.h = fh * gs;
    g_game_dst.x = hole.x + (hole.w - g_game_dst.w) * 0.5f;
    g_game_dst.y = hole.y + (hole.h - g_game_dst.h) * 0.5f;
}

/* Mouse/pen coordinates arrive in window pixels; the game's touch panel wants
 * them normalised over the game picture, not over the window. */
static void touch_map(float wx, float wy, float *nx, float *ny)
{
    float x, y;

    /* SDL window events are in logical units, the renderer draws in pixels
     * (they differ on HiDPI screens). */
    wx *= g_ev_scale_x;
    wy *= g_ev_scale_y;
    if (g_game_dst.w > 0.0f && g_game_dst.h > 0.0f) {
        x = (wx - g_game_dst.x) / g_game_dst.w;
        y = (wy - g_game_dst.y) / g_game_dst.h;
    } else {
        x = wx / (float)(g_w > 0 ? g_w : 1);
        y = wy / (float)(g_h > 0 ? g_h : 1);
    }
    *nx = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    *ny = y < 0.0f ? 0.0f : (y > 1.0f ? 1.0f : y);
}

/* OTOMEDIUS_BEZEL_DUMP=/path.ppm writes the composed window (art + game) once,
 * after OTOMEDIUS_BEZEL_DUMP_AT presents, for headless verification. */
static void bezel_dump(void)
{
    static int done;
    const char *path = getenv("OTOMEDIUS_BEZEL_DUMP");
    const char *at = getenv("OTOMEDIUS_BEZEL_DUMP_AT");
    SDL_Surface *s, *rgb;
    FILE *f;
    int x, y, w, h;
    const uint8_t *px;
    int pitch;

    if (done || !path || !*path)
        return;
    if (at && g_present_count < (unsigned)atoi(at))
        return;
    s = SDL_RenderReadPixels(g_ren, NULL);
    if (!s) {
        log_printf("platform: bezel dump failed: %s", SDL_GetError());
        done = 1;
        return;
    }
    rgb = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ARGB8888);
    SDL_DestroySurface(s);
    if (!rgb) {
        done = 1;
        return;
    }
    w = rgb->w;
    h = rgb->h;
    pitch = rgb->pitch;
    px = (const uint8_t *)rgb->pixels;
    f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) {
                const uint8_t *p = px + (size_t)y * pitch + x * 4;
                fputc(p[2], f);      /* R */
                fputc(p[1], f);      /* G */
                fputc(p[0], f);      /* B */
            }
        fclose(f);
        log_printf("platform: bezel dump %dx%d -> %s", w, h, path);
    }
    done = 1;
    SDL_DestroySurface(rgb);
}

void platform_set_size(int w, int h)
{
    if (w > 0 && h > 0) {
        g_w = w;
        g_h = h;
        /* The game creates its D3D device after the window, so follow the
         * device resolution (1280x1024 for Otomedius) instead of keeping the
         * bootstrap size.  When the cabinet art is active the window keeps the
         * artwork size and the frame is fitted into the screen hole. */
        if (g_win && !g_bezel)
            SDL_SetWindowSize(g_win, w, h);
    }
}

int platform_init(void)
{
    const char *bezel_path;
    SDL_Surface *bezel_surface = NULL;
    int win_w = 1366, win_h = 768;      /* windowed size (16:9) */

    if (g_ready)
        return 0;
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "platform: SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        fprintf(stderr, "platform: audio unavailable: %s\n", SDL_GetError());
    bezel_path = bezel_choose();
    if (bezel_path)
        bezel_surface = bezel_open(bezel_path);

    g_win = SDL_CreateWindow("Otomedius", win_w, win_h,
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
    if (bezel_surface) {
        g_bezel = SDL_CreateTextureFromSurface(g_ren, bezel_surface);
        SDL_DestroySurface(bezel_surface);
        if (g_bezel) {
            SDL_SetTextureBlendMode(g_bezel, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(g_bezel, SDL_SCALEMODE_LINEAR);
        } else {
            log_printf("platform: bezel texture failed: %s", SDL_GetError());
        }
    }
    /* The game opens fullscreen by default; F11 switches to a 1366x768
     * windowed view (OTOMEDIUS_WINDOWED=1 starts windowed). */
    if (!getenv("OTOMEDIUS_WINDOWED") || getenv("OTOMEDIUS_FULLSCREEN")) {
        g_fullscreen = 1;
        SDL_SetWindowFullscreen(g_win, true);
    }
    g_fb_w = 0;          /* the streaming texture follows render_framebuffer() */
    g_fb_h = 0;
    g_ready = 1;
    log_printf("platform: %s renderer%s%s, window %dx%d",
               SDL_GetRendererName(g_ren),
               g_bezel ? ", bezel active" : "",
               g_fullscreen ? ", fullscreen" : "", win_w, win_h);
    return 0;
}

void platform_shutdown(void)
{
    if (!g_ready)
        return;
    platform_audio_close();
    if (g_tex)
        SDL_DestroyTexture(g_tex);
    if (g_bezel)
        SDL_DestroyTexture(g_bezel);
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
    int fw = 0, fh = 0, ww = 0, wh = 0, lw = 0, lh = 0;
    const uint8_t *fb;
    SDL_Event ev;
    float nx, ny;

    if (!g_ready)
        return;

    fb = render_framebuffer(&fw, &fh);
    SDL_GetWindowSizeInPixels(g_win, &ww, &wh);
    SDL_GetWindowSize(g_win, &lw, &lh);
    if (lw > 0 && lh > 0) {
        g_ev_scale_x = (float)ww / (float)lw;
        g_ev_scale_y = (float)wh / (float)lh;
    }
    bezel_layout(ww, wh, fw, fh);

    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT) {
            log_printf("platform: SDL_EVENT_QUIT");
            movie_close();          /* stop the decoder threads before exit */
            win_request_quit();
        } else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE) {
            log_printf("platform: Esc pressed");
            movie_close();
            win_request_quit();
        } else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_F11) {
            g_fullscreen = !g_fullscreen;
            if (g_fullscreen) {
                SDL_SetWindowFullscreen(g_win, true);
            } else {
                SDL_SetWindowFullscreen(g_win, false);
                SDL_SetWindowSize(g_win, 1366, 768);
                SDL_SetWindowPosition(g_win, SDL_WINDOWPOS_CENTERED,
                                      SDL_WINDOWPOS_CENTERED);
            }
            log_printf("platform: fullscreen %s", g_fullscreen ? "on" : "off");
        } else if (ev.type == SDL_EVENT_GAMEPAD_ADDED && !g_pad) {
            g_pad = SDL_OpenGamepad(ev.gdevice.which);
        } else if (ev.type == SDL_EVENT_PEN_DOWN ||
                   ev.type == SDL_EVENT_PEN_MOTION) {
            g_touch.pen_seen = 1;
            touch_map(ev.ptouch.x, ev.ptouch.y, &nx, &ny);
            g_touch.x = nx;
            g_touch.y = ny;
            if (ev.type == SDL_EVENT_PEN_DOWN) {
                g_touch.pressed = 1;
                g_touch.pressure = 255;
            }
        } else if (ev.type == SDL_EVENT_PEN_UP) {
            g_touch.pen_seen = 1;
            touch_map(ev.ptouch.x, ev.ptouch.y, &nx, &ny);
            g_touch.x = nx;
            g_touch.y = ny;
            g_touch.pressed = 0;
            g_touch.pressure = 0;
        } else if (ev.type == SDL_EVENT_MOUSE_MOTION) {
            if (!g_touch.pen_seen) {
                touch_map(ev.motion.x, ev.motion.y, &nx, &ny);
                g_touch.x = nx;
                g_touch.y = ny;
            }
        } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            if (!g_touch.pen_seen && ev.button.button == SDL_BUTTON_LEFT) {
                touch_map(ev.button.x, ev.button.y, &nx, &ny);
                g_touch.x = nx;
                g_touch.y = ny;
                g_touch.pressed = 1;
                g_touch.pressure = 255;
            }
        } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            if (!g_touch.pen_seen && ev.button.button == SDL_BUTTON_LEFT) {
                g_touch.pressed = 0;
                g_touch.pressure = 0;
            }
        }
    }

    if (fb && fw > 0 && fh > 0) {
        if (!g_tex || g_fb_w != fw || g_fb_h != fh) {
            if (g_tex)
                SDL_DestroyTexture(g_tex);
            g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888,
                                      SDL_TEXTUREACCESS_STREAMING, fw, fh);
            if (g_tex)
                SDL_SetTextureScaleMode(g_tex, SDL_SCALEMODE_LINEAR);
            g_fb_w = fw;
            g_fb_h = fh;
        }
        if (g_tex)
            SDL_UpdateTexture(g_tex, NULL, fb, fw * 4);
    }

    SDL_SetRenderDrawColor(g_ren, (g_clear >> 16) & 0xFF, (g_clear >> 8) & 0xFF,
                           g_clear & 0xFF, 255);
    SDL_RenderClear(g_ren);

    /* The cabinet art is 1920x1080 with a transparent screen area; the game
     * frame goes into that hole.  If the art had no transparent hole, it is
     * drawn first and the frame covers its middle. */
    if (g_bezel && !g_bezel_overlay)
        SDL_RenderTexture(g_ren, g_bezel, NULL, &g_bezel_dst);
    if (g_tex)
        SDL_RenderTexture(g_ren, g_tex, NULL,
                          g_bezel ? &g_game_dst : NULL);
    if (g_bezel && g_bezel_overlay)
        SDL_RenderTexture(g_ren, g_bezel, NULL, &g_bezel_dst);

    bezel_dump();          /* read back the composed back buffer */
    SDL_RenderPresent(g_ren);
    g_present_count++;
}

void platform_wait(void)
{
    if (g_ready)
        SDL_WaitEventTimeout(NULL, 1);
}

/* ---------------------------------------------------------------- audio */
int platform_audio_open(int rate, int channels)
{
    SDL_AudioSpec spec;

    if (rate <= 0 || channels <= 0)
        return -1;
    if (g_astream && g_arate == rate && g_achannels == channels)
        return 0;
    platform_audio_close();
    SDL_zero(spec);
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = channels;
    spec.freq = rate;
    g_astream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                          &spec, NULL, NULL);
    if (!g_astream) {
        log_printf("platform: audio device failed: %s", SDL_GetError());
        return -1;
    }
    g_arate = rate;
    g_achannels = channels;
    SDL_ResumeAudioStreamDevice(g_astream);
    log_printf("platform: audio %d Hz %d ch", rate, channels);
    return 0;
}

void platform_audio_write(const void *data, int len)
{
    if (!g_astream || !data || len <= 0)
        return;
    /* Do not drift ahead of the video: ~200 ms of queued audio is plenty.
     * The wait is bounded (and the chunk is dropped when the device stops
     * consuming) so a stalled audio device can never block shutdown. */
    {
        int spins = 0;
        while (SDL_GetAudioStreamQueued(g_astream) >
               g_arate * g_achannels * 2 / 5) {
            if (++spins > 100)
                return;             /* device stalled: drop this chunk */
            SDL_Delay(5);
        }
    }
    SDL_PutAudioStreamData(g_astream, data, len);
}

void platform_audio_close(void)
{
    if (g_astream) {
        SDL_DestroyAudioStream(g_astream);
        g_astream = NULL;
    }
    g_arate = g_achannels = 0;
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
{    memset(out, 0, sizeof(*out));
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

void platform_read_touch(touch_state_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!g_ready)
        return;
    out->x = g_touch.x < 0.0f ? 0.0f : (g_touch.x > 1.0f ? 1.0f : g_touch.x);
    out->y = g_touch.y < 0.0f ? 0.0f : (g_touch.y > 1.0f ? 1.0f : g_touch.y);
    out->pressed = g_touch.pressed;
    out->pressure = g_touch.pressure;
}
