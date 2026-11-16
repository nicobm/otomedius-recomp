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

#include <string.h>

/* --------------------------------------------------------------- objects */
typedef struct {
    uint32_t vt, refcount;
    uint32_t cap;          /* bytes of sample data */
    uint32_t data;         /* emulated pointer to the samples */
    uint32_t playing;
    int32_t  volume, pan;
    uint32_t freq;
    uint32_t position;
} dsbuffer_t;

static dsbuffer_t *dsb(uint32_t o) { return (dsbuffer_t *)(MEM + o); }
static uint32_t dsbuffer_create(uint32_t bytes);

/* ------------------------------------------------------- IDirectSound8 */
static void ds_QueryInterface(void) { RET32(0); }
static void ds_AddRef(void) { M32(arg32(0) + 4)++; RET32(M32(arg32(0) + 4)); }
static void ds_Release(void) { RET32(1); }

static void ds_CreateSoundBuffer(void)
{
    uint32_t desc = arg32(1), ppbuf = arg32(2);
    uint32_t size = 0;
    if (desc) {
        /* DSBUFFERDESC: dwSize, dwFlags, dwBufferBytes, ... */
        size = M32(desc + 8);
        if (!size)
            size = 1u << 20;   /* DSBCAPS_PRIMARYBUFFER */
    }
    uint32_t obj = dsbuffer_create(size);
    if (ppbuf)
        M32(ppbuf) = obj;
    RET32(0);
}

static void ds_GetCaps(void) { RET32(0); }
static void ds_DuplicateSoundBuffer(void) { RET32(0x80004001u); }
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
static void b_QueryInterface(void) { RET32(0); }
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

static void b_GetFormat(void) { RET32(0); }

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
    if (p)
        M32(p) = dsb(arg32(0))->playing ? 1u : 0u;
    RET32(0);
}

static void b_Initialize(void) { RET32(0); }

static void b_Lock(void)
{
    dsbuffer_t *b = dsb(arg32(0));
    uint32_t offset = arg32(1), bytes = arg32(2);
    uint32_t p1 = arg32(3), n1 = arg32(4), p2 = arg32(5), n2 = arg32(6);
    uint32_t start = b->data + (b->cap ? offset % b->cap : 0);
    if (p1)
        M32(p1) = start;
    if (n1)
        M32(n1) = bytes;
    if (p2)
        M32(p2) = 0;
    if (n2)
        M32(n2) = 0;
    RET32(0);
}

static void b_Play(void)
{
    dsb(arg32(0))->playing = 1;
    RET32(0);
}

static void b_Stop(void)
{
    dsb(arg32(0))->playing = 0;
    RET32(0);
}

static void b_SetCurrentPosition(void)
{
    dsb(arg32(0))->position = arg32(1);
    RET32(0);
}

static void b_SetFormat(void) { RET32(0); }

static void b_SetVolume(void)
{
    dsb(arg32(0))->volume = (int32_t)arg32(1);
    RET32(0);
}

static void b_SetPan(void)
{
    dsb(arg32(0))->pan = (int32_t)arg32(1);
    RET32(0);
}

static void b_SetFrequency(void)
{
    dsb(arg32(0))->freq = arg32(1);
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
    return obj;
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
