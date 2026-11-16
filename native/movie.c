/* movie.c - decode the opening movie with the runtime's ffmpeg.
 *
 * The arcade dump ships Data/OTOMEDIUS_OP_f.wmv (WMV3 640x512 30 fps, 60 s).
 * The base freedesktop runtime has the ffmpeg binary and libavcodec (wmav2 and
 * the containers) but the WMV3 decoder lives in the org.freedesktop.Platform
 * codecs-extra extension, mounted at /usr/lib/<arch>/codecs-extra/lib.  The
 * launcher prepends that directory to LD_LIBRARY_PATH; without the extension
 * ffmpeg logs "no decoder found for wmv3" and this module simply reports no
 * frames (the port keeps working, the video stays blank).
 *
 * ffmpeg is run with -re so it emits frames at the native rate; a reader
 * thread stores the newest frame in a small ring and the game copies it into
 * its D3D video texture (or the renderer draws it as a preview overlay).
 */

#define _GNU_SOURCE
#include "win32.h"
#include "movie.h"
#include "platform.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MOVIE_SLOTS 3

static int g_w, g_h, g_fps;
static size_t g_frame_size;
static uint8_t *g_slot[MOVIE_SLOTS];
static int g_write_slot, g_latest = -1;
static volatile int g_serial, g_shown;
static volatile int g_run, g_eof;
static int g_fd = -1;
static pid_t g_pid = -1;
static pthread_t g_thread;
static int g_thread_started;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_path[4096];

/* Audio: the same file, decoded to 48 kHz stereo PCM and pushed to the
 * platform backend.  It runs as a second ffmpeg process so the video pipe
 * keeps its raw-frame cadence. */
#define MOVIE_AUDIO_RATE 48000
#define MOVIE_AUDIO_CHANNELS 2
static int g_afd = -1;
static pid_t g_apid = -1;
static pthread_t g_athread;
static int g_athread_started;
static volatile int g_arun;

static void *movie_reader(void *arg)
{
    size_t got = 0;

    (void)arg;
    while (g_run) {
        uint8_t *dst = g_slot[g_write_slot];
        ssize_t n = read(g_fd, dst + got, g_frame_size - got);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        got += (size_t)n;
        if (got == g_frame_size) {
            got = 0;
            pthread_mutex_lock(&g_lock);
            g_latest = g_write_slot;
            g_serial++;
            pthread_mutex_unlock(&g_lock);
            g_write_slot = (g_write_slot + 1) % MOVIE_SLOTS;
        }
    }
    pthread_mutex_lock(&g_lock);
    g_eof = 1;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

static void *movie_audio_reader(void *arg)
{
    static uint8_t buf[8192];

    (void)arg;
    while (g_arun) {
        ssize_t n = read(g_afd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        platform_audio_write(buf, (int)n);
    }
    return NULL;
}

/* Spawn the audio decoder; returns 0 when samples will flow. */
static int movie_audio_start(const char *path)
{
    int fds[2];
    pid_t pid;

    if (platform_audio_open(MOVIE_AUDIO_RATE, MOVIE_AUDIO_CHANNELS) != 0)
        return -1;
    if (pipe(fds) != 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        if (dup2(fds[1], 1) < 0)
            _exit(126);
        close(fds[1]);
        execlp("ffmpeg", "ffmpeg", "-v", "error", "-nostdin", "-re",
               "-i", path, "-vn", "-f", "s16le", "-ar", "48000", "-ac", "2",
               "-", (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    g_afd = fds[0];
    g_apid = pid;
    g_arun = 1;
    if (pthread_create(&g_athread, NULL, movie_audio_reader, NULL) != 0) {
        g_arun = 0;
        kill(g_apid, SIGKILL);
        waitpid(g_apid, NULL, 0);
        g_apid = -1;
        close(g_afd);
        g_afd = -1;
        return -1;
    }
    g_athread_started = 1;
    return 0;
}

int movie_open(const char *path, int width, int height, int fps)
{
    int fds[2];
    pid_t pid;

    movie_close();
    if (!path || !*path || width <= 0 || height <= 0)
        return -1;
    g_w = width;
    g_h = height;
    g_fps = fps > 0 ? fps : 30;
    g_frame_size = (size_t)width * (size_t)height * 4;
    snprintf(g_path, sizeof(g_path), "%s", path);

    for (int i = 0; i < MOVIE_SLOTS; i++) {
        g_slot[i] = (uint8_t *)malloc(g_frame_size);
        if (!g_slot[i]) {
            movie_close();
            return -1;
        }
        memset(g_slot[i], 0, g_frame_size);
    }
    if (pipe(fds) != 0) {
        movie_close();
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        movie_close();
        return -1;
    }
    if (pid == 0) {
        /* child: decode to raw BGRA on stdout */
        close(fds[0]);
        if (dup2(fds[1], 1) < 0)
            _exit(126);
        close(fds[1]);
        execlp("ffmpeg", "ffmpeg", "-v", "error", "-nostdin", "-re",
               "-i", path, "-f", "rawvideo", "-pix_fmt", "bgra", "-",
               (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    g_fd = fds[0];
    g_pid = pid;
    g_write_slot = 0;
    g_latest = -1;
    g_serial = 0;
    g_shown = -1;
    g_eof = 0;
    g_run = 1;
    if (pthread_create(&g_thread, NULL, movie_reader, NULL) != 0) {
        g_run = 0;
        kill(g_pid, SIGKILL);
        waitpid(g_pid, NULL, 0);
        g_pid = -1;
        close(g_fd);
        g_fd = -1;
        movie_close();
        return -1;
    }
    g_thread_started = 1;
    movie_audio_start(path);
    log_printf("movie: decoding %s (%dx%d, %d fps) with ffmpeg", path,
               g_w, g_h, g_fps);
    return 0;
}

void movie_close(void)
{
    if (g_athread_started) {
        g_arun = 0;
        if (g_apid > 0)
            kill(g_apid, SIGKILL);
        pthread_join(g_athread, NULL);
        g_athread_started = 0;
    }
    if (g_apid > 0) {
        waitpid(g_apid, NULL, 0);
        g_apid = -1;
    }
    if (g_afd >= 0) {
        close(g_afd);
        g_afd = -1;
    }
    platform_audio_close();
    if (g_thread_started) {
        g_run = 0;
        if (g_pid > 0)
            kill(g_pid, SIGKILL);       /* closes the pipe, wakes the reader */
        pthread_join(g_thread, NULL);
        g_thread_started = 0;
    }
    if (g_pid > 0) {
        waitpid(g_pid, NULL, 0);
        g_pid = -1;
    }
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
    for (int i = 0; i < MOVIE_SLOTS; i++) {
        free(g_slot[i]);
        g_slot[i] = NULL;
    }
    g_latest = -1;
    g_eof = 0;
    g_serial = g_shown = 0;
    g_w = g_h = 0;
    g_frame_size = 0;
}

int movie_active(void) { return g_thread_started || g_latest >= 0; }
int movie_finished(void) { return g_eof && g_serial == g_shown; }

double movie_position(void)
{
    return g_fps > 0 ? (double)g_shown / (double)g_fps : 0.0;
}

/* nearest-neighbour scale of one BGRA frame; alpha is forced opaque because
 * the game draws the video with a plain textured quad */
int movie_frame(uint8_t *dst, int dst_w, int dst_h)
{
    const uint8_t *src;
    int idx, serial;

    if (!dst || dst_w <= 0 || dst_h <= 0 || !g_w || !g_h)
        return 0;
    pthread_mutex_lock(&g_lock);
    idx = g_latest;
    serial = g_serial;
    pthread_mutex_unlock(&g_lock);
    if (idx < 0 || serial == g_shown)
        return 0;
    g_shown = serial;
    src = g_slot[idx];

    if (dst_w == g_w * 2 && dst_h == g_h * 2) {
        /* the common case: 640x512 into the 1280x1024 arcade texture */
        for (int y = 0; y < g_h; y++) {
            const uint32_t *s = (const uint32_t *)(src +
                                  (size_t)y * g_w * 4);
            uint32_t *d0 = (uint32_t *)(dst + (size_t)(y * 2) * dst_w * 4);
            uint32_t *d1 = (uint32_t *)(dst + (size_t)(y * 2 + 1) * dst_w * 4);
            for (int x = 0; x < g_w; x++) {
                uint32_t c = s[x] | 0xFF000000u;
                d0[x * 2] = c;
                d0[x * 2 + 1] = c;
                d1[x * 2] = c;
                d1[x * 2 + 1] = c;
            }
        }
        return 1;
    }

    for (int y = 0; y < dst_h; y++) {
        int sy = (int)((int64_t)y * g_h / dst_h);
        const uint32_t *s = (const uint32_t *)(src + (size_t)sy * g_w * 4);
        uint32_t *d = (uint32_t *)(dst + (size_t)y * dst_w * 4);
        for (int x = 0; x < dst_w; x++) {
            int sx = (int)((int64_t)x * g_w / dst_w);
            d[x] = s[sx] | 0xFF000000u;
        }
    }
    return 1;
}
