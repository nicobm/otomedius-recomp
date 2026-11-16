/* dsound.c - DirectSound 8 replacement.
 *
 * The game opens a DirectSound device, creates secondary buffers, locks them
 * to write sample data and plays them.  All of that is implemented here on
 * top of the emulated address space; the mixer that turns those buffers into
 * sound is a separate step (audio is silent until then).
 */

#define _GNU_SOURCE
#include "win32.h"
#include "d3d9.h"
#include "platform.h"
#include "movie.h"

#include <string.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>

/* --------------------------------------------------------------- objects */
typedef struct {
    uint32_t vt, refcount;
    uint32_t cap;          /* bytes of sample data */
    uint32_t data;         /* emulated pointer to the samples */
    uint32_t playing;
    int32_t  volume, pan;
    uint32_t freq;
    uint32_t position;
    /* WAVEFORMATEX copied from DSBUFFERDESC/SetFormat */
    uint32_t channels, rate, bits, format_tag, is_primary;
    uint32_t play_flags;
    double   phase;        /* fractional source frame kept between blocks */
} dsbuffer_t;

static dsbuffer_t *dsb(uint32_t o) { return (dsbuffer_t *)(MEM + o); }
static uint32_t dsbuffer_create(uint32_t bytes);
static uint32_t dsbuffer_clone(uint32_t src);
static int dsdbg(void);
static void ds_registry_add(uint32_t obj);
static void mix_start(void);

/* ------------------------------------------------------- IDirectSound8 */
static void ds_QueryInterface(void) { RET32(0); }
static void ds_AddRef(void) { M32(arg32(0) + 4)++; RET32(M32(arg32(0) + 4)); }
static void ds_Release(void) { RET32(1); }

static void ds_CreateSoundBuffer(void)
{
    uint32_t desc = arg32(1), ppbuf = arg32(2);
    uint32_t size = 0, flags = 0, wfx = 0;
    if (desc) {
        /* DSBUFFERDESC: dwSize, dwFlags, dwBufferBytes, dwReserved, lpwfxFormat */
        size = M32(desc + 8);
        flags = M32(desc + 4);
        wfx = M32(desc + 0x10);
        if (!size)
            size = 1u << 20;   /* DSBCAPS_PRIMARYBUFFER */
    }
    uint32_t obj = dsbuffer_create(size);
    if (obj && desc) {
        dsbuffer_t *b = dsb(obj);
        b->is_primary = flags & 1;
        if (wfx) {
            b->format_tag = M16(wfx + 0);
            b->channels = M16(wfx + 2);
            b->rate = M32(wfx + 4);
            b->bits = M16(wfx + 14);
            b->freq = b->rate;
        }
        if (dsdbg())
            fprintf(stderr, "dsound: CreateSoundBuffer(%u bytes flags=%x "
                    "fmt=%u ch=%u rate=%u bits=%u)\n", size, flags,
                    b->format_tag, b->channels, b->rate, b->bits);
    }
    if (ppbuf)
        M32(ppbuf) = obj;
    RET32(0);
}

static void ds_GetCaps(void) { RET32(0); }
static void ds_DuplicateSoundBuffer(void)
{
    /* The sound engine duplicates a wave's buffer for each voice (0x41BE20
     * calls this through vtbl+0x14) and expects the duplicate in the third
     * argument.  Returning E_NOTIMPL left the voice's buffer pointer at NULL
     * while the voice was still linked and queued, so the mixer then called
     * through a NULL vtable (0x41BFB8).  The clone shares the sample memory;
     * volume/pan/position stay per object. */
    uint32_t src = arg32(1), ppbuf = arg32(2);
    uint32_t obj = src ? dsbuffer_clone(src) : dsbuffer_create(0);
    if (ppbuf)
        M32(ppbuf) = obj;
    RET32(0);
}
static void ds_SetCooperativeLevel(void) { RET32(0); }
static void ds_Compact(void) { }
static void ds_GetSpeakerConfig(void)
{
    uint32_t p = arg32(1);
    if (p)
        M32(p) = 2;             /* DSSPEAKER_STEREO */
    RET32(0);
}
static void ds_SetSpeakerConfig(void) { RET32(0); }
static void ds_Initialize(void) { RET32(0); }
static void ds_VerifyCertification(void)
{
    uint32_t p = arg32(1);
    if (p)
        M32(p) = 1;
    RET32(0);
}

static const method_t dsound_methods[] = {
    {ds_QueryInterface, 3},   /* QueryInterface */
    {ds_AddRef, 1},   /* AddRef */
    {ds_Release, 1},   /* Release */
    {ds_CreateSoundBuffer, 4},   /* CreateSoundBuffer */
    {ds_GetCaps, 2},   /* GetCaps */
    {ds_DuplicateSoundBuffer, 3},   /* DuplicateSoundBuffer */
    {ds_SetCooperativeLevel, 3},   /* SetCooperativeLevel */
    {ds_Compact, 1},   /* Compact */
    {ds_GetSpeakerConfig, 2},   /* GetSpeakerConfig */
    {ds_SetSpeakerConfig, 2},   /* SetSpeakerConfig */
    {ds_Initialize, 2},   /* Initialize */
    {ds_VerifyCertification, 2},   /* VerifyCertification */
};

/* --------------------------------------------------- IDirectSoundBuffer8 */
static int guid_eq(uint32_t p, const uint8_t *g)
{
    return p && memcmp(MEM + p, g, 16) == 0;
}
static const uint8_t guid_iunknown[16] = {
    0x00,0x00,0x00,0x00, 0x00,0x00, 0x00,0x00,
    0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 };
static const uint8_t guid_dsbuffer[16] = {
    0x85,0xfa,0x9a,0x27, 0x81,0x49, 0xce,0x11,
    0xa5,0x21,0x00,0x20,0xaf,0x0b,0xe5,0x60 };
static const uint8_t guid_dsbuffer8[16] = {
    0x49,0xa4,0x25,0x68, 0x24,0x75, 0x82,0x4d,
    0x92,0x00,0x50,0xdc,0x6c,0x4b,0x66,0xe7 };

static void b_QueryInterface(void)
{
    uint32_t this_ = arg32(0), riid = arg32(1), ppv = arg32(2);
    if (ppv)
        M32(ppv) = 0;
    if (guid_eq(riid, guid_iunknown) || guid_eq(riid, guid_dsbuffer) ||
        guid_eq(riid, guid_dsbuffer8)) {
        if (ppv)
            M32(ppv) = this_;
        M32(this_ + 4)++;
        RET32(0);
        return;
    }
    /* IDirectSoundNotify needs an extra method; the game degrades cleanly
     * when it is not offered. */
    RET32(0x80004002u);        /* E_NOINTERFACE */
}
static void b_AddRef(void) { M32(arg32(0) + 4)++; RET32(M32(arg32(0) + 4)); }
static void b_Release(void) { RET32(1); }
static void b_GetCaps(void) { RET32(0); }

static void b_GetCurrentPosition(void)
{
    dsbuffer_t *b = dsb(arg32(0));
    uint32_t play = arg32(1), write = arg32(2);
    if (play)
        M32(play) = b->position % (b->cap ? b->cap : 1);
    if (write)
        M32(write) = 0;
    RET32(0);
}

static void b_GetFormat(void)
{
    uint32_t o = arg32(0), p = arg32(1);
    dsbuffer_t *b = dsb(o);
    if (p) {
        M16(p + 0) = (uint16_t)b->format_tag;
        M16(p + 2) = (uint16_t)b->channels;
        M32(p + 4) = b->rate;
        M32(p + 8) = b->rate * b->channels * (b->bits / 8);
        M16(p + 12) = (uint16_t)(b->channels * (b->bits / 8));
        M16(p + 14) = (uint16_t)b->bits;
        M16(p + 16) = 0;
    }
    RET32(0);
}

static void b_GetVolume(void)
{
    uint32_t p = arg32(1);
    if (p)
        M32(p) = (uint32_t)dsb(arg32(0))->volume;
    RET32(0);
}

static void b_GetPan(void)
{
    uint32_t p = arg32(1);
    if (p)
        M32(p) = (uint32_t)dsb(arg32(0))->pan;
    RET32(0);
}

static void b_GetFrequency(void)
{
    uint32_t p = arg32(1);
    if (p)
        M32(p) = dsb(arg32(0))->freq;
    RET32(0);
}

static void b_GetStatus(void)
{
    uint32_t p = arg32(1);
    dsbuffer_t *b = dsb(arg32(0));
    if (p)
        M32(p) = (b->playing ? 1u : 0u) |
                 (b->playing && (b->play_flags & 1) ? 4u : 0u);
    RET32(0);
}

static void b_Initialize(void) { RET32(0); }

static void b_Lock(void)
{
    dsbuffer_t *b = dsb(arg32(0));
    uint32_t offset = arg32(1), bytes = arg32(2);
    uint32_t p1 = arg32(3), n1 = arg32(4), p2 = arg32(5), n2 = arg32(6);
    uint32_t cap = b->cap ? b->cap : 1;
    uint32_t first = bytes;

    offset %= cap;
    if (offset + bytes > cap)
        first = cap - offset;          /* wrap-around lock */
    if (p1)
        M32(p1) = b->data + offset;
    if (n1)
        M32(n1) = first;
    if (p2) {
        M32(p2) = first < bytes ? b->data : 0;
        if (n2)
            M32(n2) = first < bytes ? bytes - first : 0;
    } else if (n2) {
        M32(n2) = 0;
    }
    if (dsdbg()) {
        static unsigned n;
        if (n++ < 40)
            fprintf(stderr, "dsound: Lock(obj=%08x off=%u bytes=%u cap=%u)\n",
                    arg32(0), offset, bytes, b->cap);
    }
    RET32(0);
}

static void b_Play(void)
{
    dsbuffer_t *b = dsb(arg32(0));
    b->play_flags = arg32(1);
    b->playing = 1;
    mix_start();
    if (dsdbg()) {
        static unsigned n;
        if (n++ < 40)
            fprintf(stderr, "dsound: Play(obj=%08x flags=%u) cap=%u rate=%u "
                    "ch=%u bits=%u\n", arg32(0), arg32(1), b->cap, b->freq,
                    b->channels, b->bits);
    }
    RET32(0);
}

static void b_Stop(void)
{
    dsbuffer_t *b = dsb(arg32(0));
    b->playing = 0;
    b->position = 0;
    b->phase = 0;
    if (dsdbg()) {
        static unsigned n;
        if (n++ < 40)
            fprintf(stderr, "dsound: Stop(obj=%08x)\n", arg32(0));
    }
    RET32(0);
}

static void b_SetCurrentPosition(void)
{
    dsbuffer_t *b = dsb(arg32(0));
    b->position = arg32(1);
    b->phase = 0;
    if (b->position > b->cap)
        b->position = 0;
    RET32(0);
}

static void b_SetFormat(void)
{
    dsbuffer_t *b = dsb(arg32(0));
    uint32_t p = arg32(1);
    if (p) {
        b->format_tag = M16(p + 0);
        b->channels = M16(p + 2);
        b->rate = M32(p + 4);
        b->bits = M16(p + 14);
        b->freq = b->rate;
        if (dsdbg())
            fprintf(stderr, "dsound: SetFormat(obj=%08x fmt=%u ch=%u rate=%u "
                    "bits=%u)\n", arg32(0), b->format_tag, b->channels,
                    b->rate, b->bits);
    }
    RET32(0);
}

static void b_SetVolume(void)
{
    dsb(arg32(0))->volume = (int32_t)arg32(1);
    if (dsdbg()) {
        static unsigned n;
        if (n++ < 40)
            fprintf(stderr, "dsound: SetVolume(obj=%08x vol=%d)\n",
                    arg32(0), (int32_t)arg32(1));
    }
    RET32(0);
}

static void b_SetPan(void)
{
    dsb(arg32(0))->pan = (int32_t)arg32(1);
    RET32(0);
}

static void b_SetFrequency(void)
{
    dsbuffer_t *b = dsb(arg32(0));
    b->freq = arg32(1) ? arg32(1) : b->rate;
    if (dsdbg()) {
        static unsigned n;
        if (n++ < 40)
            fprintf(stderr, "dsound: SetFrequency(obj=%08x freq=%u)\n",
                    arg32(0), b->freq);
    }
    RET32(0);
}

static void b_Unlock(void) { RET32(0); }
static void b_Restore(void) { RET32(0); }
static void b_SetFX(void) { RET32(0x80004005u); }        /* E_FAIL */
static void b_AcquireResources(void) { RET32(0); }
static void b_GetObjectInPath(void) { RET32(0x80004005u); }

static const method_t dsbuffer_methods[] = {
    {b_QueryInterface, 3},   /* QueryInterface */
    {b_AddRef, 1},   /* AddRef */
    {b_Release, 1},   /* Release */
    {b_GetCaps, 2},   /* GetCaps */
    {b_GetCurrentPosition, 3},   /* GetCurrentPosition */
    {b_GetFormat, 4},   /* GetFormat */
    {b_GetVolume, 2},   /* GetVolume */
    {b_GetPan, 2},   /* GetPan */
    {b_GetFrequency, 2},   /* GetFrequency */
    {b_GetStatus, 2},   /* GetStatus */
    {b_Initialize, 3},   /* Initialize */
    {b_Lock, 8},   /* Lock */
    {b_Play, 4},   /* Play */
    {b_SetCurrentPosition, 2},   /* SetCurrentPosition */
    {b_SetFormat, 2},   /* SetFormat */
    {b_SetVolume, 2},   /* SetVolume */
    {b_SetPan, 2},   /* SetPan */
    {b_SetFrequency, 2},   /* SetFrequency */
    {b_Stop, 1},   /* Stop */
    {b_Unlock, 5},   /* Unlock */
    {b_Restore, 1},   /* Restore */
    {b_SetFX, 4},   /* SetFX */
    {b_AcquireResources, 4},   /* AcquireResources */
    {b_GetObjectInPath, 5},   /* GetObjectInPath */
};

static uint32_t dsbuffer_create(uint32_t bytes)
{
    uint32_t obj = com_create_named("IDirectSoundBuffer8", dsbuffer_methods,
                              (int)(sizeof(dsbuffer_methods) / sizeof(method_t)),
                              sizeof(dsbuffer_t));
    dsbuffer_t *b = dsb(obj);
    b->cap = bytes ? bytes : 0x1000;
    b->data = emu_alloc(b->cap + 64);
    b->volume = 0;
    b->freq = 44100;
    b->channels = 2;
    b->rate = 44100;
    b->bits = 16;
    ds_registry_add(obj);
    return obj;
}

/* Duplicate without allocating sample memory: the clone shares the source's
 * buffer, exactly what the sound engine uses it for. */
static uint32_t dsbuffer_clone(uint32_t src_obj)
{
    /* share the source's vtable: registering a second set of method thunks
     * for every voice would burn the thunk table and race with the mixer */
    extern uint32_t emu_alloc(uint32_t size);
    uint32_t obj = emu_alloc(8u + sizeof(dsbuffer_t));
    M32(obj) = M32(src_obj);
    M32(obj + 4) = 1;
    dsbuffer_t *d = dsb(obj), *s = dsb(src_obj);
    d->cap = s->cap;
    d->data = s->data;
    d->volume = s->volume;
    d->pan = s->pan;
    d->freq = s->freq;
    d->channels = s->channels;
    d->rate = s->rate;
    d->bits = s->bits;
    d->format_tag = s->format_tag;
    d->is_primary = s->is_primary;
    ds_registry_add(obj);
    return obj;
}

/* ------------------------------------------------------------- mixer
 * The game's sound engine gives every voice its own IDirectSoundBuffer8
 * (DuplicateSoundBuffer, sharing the wave's sample memory) and lets DirectSound
 * do the mixing.  There is no hardware here, so a host thread mixes every
 * playing buffer into one 48 kHz stereo S16 stream and feeds SDL.  Advancing
 * dsbuffer_t.position is required: 0x41B2A0 polls GetCurrentPosition and calls
 * Stop when it passes the sample length, so without it voices never end. */
#define MIX_RATE   48000
#define MIX_FRAMES 480
#define MAX_DSB    4096

static uint32_t g_bufs[MAX_DSB];
static int g_nbufs;
static pthread_mutex_t g_mix_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_mix_thread;
static volatile int g_mix_started, g_mix_stop;

static int dsdbg(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("OTOMEDIUS_DSDBG") != NULL;
    return on;
}

static void ds_registry_add(uint32_t obj)
{
    pthread_mutex_lock(&g_mix_lock);
    if (g_nbufs < MAX_DSB)
        g_bufs[g_nbufs++] = obj;
    pthread_mutex_unlock(&g_mix_lock);
}

/* DirectSound volume is hundredths of a decibel: 0 = full, -10000 = silence. */
static double ds_gain(int32_t vol)
{
    if (vol <= -10000)
        return 0.0;
    if (vol >= 0)
        return 1.0;
    return pow(10.0, (double)vol / 2000.0);
}

static void mix_buffer(dsbuffer_t *b, float *acc, int frames)
{
    uint32_t ch, bits, rate, bps, bpf, total, i;
    double step, pos, gain, gl, gr;

    if (!b->playing || !b->cap || b->is_primary)
        return;
    ch = b->channels ? b->channels : 2;
    bits = b->bits ? b->bits : 16;
    rate = b->freq ? b->freq : (b->rate ? b->rate : 44100);
    if (ch < 1) ch = 1;
    if (ch > 2) ch = 2;
    if (bits != 8 && bits != 16)
        return;
    bps = bits / 8;
    bpf = ch * bps;
    total = b->cap / bpf;
    if (!total)
        return;

    step = (double)rate / (double)MIX_RATE;
    pos = (double)b->position / (double)bpf + b->phase;
    gain = ds_gain(b->volume);
    gl = gr = gain;
    if (b->pan < 0)
        gr *= (10000.0 + (double)b->pan) / 10000.0;
    else if (b->pan > 0)
        gl *= (10000.0 - (double)b->pan) / 10000.0;

    for (i = 0; i < (uint32_t)frames; i++) {
        double l, r;
        uint32_t off;
        if (pos >= (double)total) {
            if (b->play_flags & 1)      /* DSBPLAY_LOOPING */
                pos -= (double)total * floor(pos / (double)total);
            else {
                b->playing = 0;
                b->position = 0;
                b->phase = 0;
                return;
            }
        }
        off = b->data + (uint32_t)pos * bpf;
        if (bits == 16) {
            int16_t s0 = (int16_t)((uint16_t)MEM[off] |
                                   ((uint16_t)MEM[off + 1] << 8));
            l = (double)s0 / 32768.0;
            if (ch == 2) {
                int16_t s1 = (int16_t)((uint16_t)MEM[off + 2] |
                                       ((uint16_t)MEM[off + 3] << 8));
                r = (double)s1 / 32768.0;
            } else {
                r = l;
            }
        } else {
            l = ((int)MEM[off] - 128) / 128.0;
            r = ch == 2 ? ((int)MEM[off + 1] - 128) / 128.0 : l;
        }
        acc[i * 2] += (float)(l * gl);
        acc[i * 2 + 1] += (float)(r * gr);
        pos += step;
    }
    b->position = (uint32_t)(pos * (double)bpf);
    b->phase = pos - floor(pos);
    if (b->position >= b->cap) {
        if (b->play_flags & 1)
            b->position %= b->cap;
        else {
            b->playing = 0;
            b->position = 0;
            b->phase = 0;
        }
    }
}

static void mix_block(int16_t *out, int frames)
{
    float acc[MIX_FRAMES * 2];
    int k, i;

    if (frames > MIX_FRAMES)
        frames = MIX_FRAMES;
    memset(acc, 0, (size_t)frames * 2 * sizeof(float));
    pthread_mutex_lock(&g_mix_lock);
    for (k = 0; k < g_nbufs; k++)
        mix_buffer(dsb(g_bufs[k]), acc, frames);
    pthread_mutex_unlock(&g_mix_lock);
    for (i = 0; i < frames * 2; i++) {
        float v = acc[i] * 32767.0f;
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        out[i] = (int16_t)v;
    }
}

static void *mix_thread(void *arg)
{
    int16_t out[MIX_FRAMES * 2];
    const char *dpath = getenv("OTOMEDIUS_AUDIO_DUMP");
    FILE *dump = dpath ? fopen(dpath, "wb") : NULL;
    int have_audio;
    (void)arg;
    have_audio = platform_audio_open(MIX_RATE, 2) == 0;
    if (!have_audio)
        log_printf("dsound: no audio device; mixing silently");
    else
        log_printf("dsound: mixer -> SDL %d Hz stereo", MIX_RATE);
    if (dump)
        fprintf(stderr, "dsound: dumping mixed audio to %s (s16le %d Hz stereo)\n",
                dpath, MIX_RATE);
    while (!g_mix_stop) {
        mix_block(out, MIX_FRAMES);
        /* The intro movie feeds the same SDL stream from its own decoder
         * thread; mixing dsound on top only steals queue space and makes the
         * song choppy.  Keep advancing the voices (0x41B2A0 polls their
         * position) but do not output while the movie owns the audio. */
        if (!movie_active()) {
            if (dump)
                fwrite(out, sizeof out, 1, dump);
            platform_audio_write(out, (int)sizeof out);
        }
        /* Without a device there is nothing to throttle us, so keep the
         * emulated play position at real time. */
        usleep(have_audio ? 2000 : 10000);
    }
    if (dump)
        fclose(dump);
    return NULL;
}

static void mix_start(void)
{
    if (g_mix_started)
        return;
    g_mix_stop = 0;
    g_mix_started = 1;
    if (pthread_create(&g_mix_thread, NULL, mix_thread, NULL) != 0) {
        g_mix_started = 0;
        log_printf("dsound: could not start the mixer thread");
    }
}

void dsound_shutdown(void)
{
    if (!g_mix_started)
        return;
    g_mix_stop = 1;
    pthread_join(g_mix_thread, NULL);
    g_mix_started = 0;
}

/* ------------------------------------------------------------ entry point */
static void DirectSoundCreate8(void)
{
    uint32_t ppds = arg32(1);   /* DirectSoundCreate8(guid, ppDS8, pUnkOuter) */
    uint32_t obj = com_create_named("IDirectSound8", dsound_methods,
                              (int)(sizeof(dsound_methods) / sizeof(method_t)), 8);
    if (ppds)
        M32(ppds) = obj;
    log_printf("dsound: DirectSoundCreate8 -> %08x", obj);
    RET32(0);
}

const import_def_t dsound_imports[] = {
    {"dsound.dll", "ordinal:11", DirectSoundCreate8, 3},
    {NULL, NULL, NULL, 0},
};
