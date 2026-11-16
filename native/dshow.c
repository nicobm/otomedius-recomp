/* dshow.c - just enough DirectShow for the game's opening movie.
 *
 * Otomedius creates a filter graph to play its opening video.  We do not
 * decode WMV here: the graph accepts the file and reports the movie as
 * finished immediately, which is what the game waits for before continuing.
 * Every interface is a real COM object with the exact vtable layout taken
 * from the DirectShow headers (wrong argument counts corrupt the stack).
 */

#define _GNU_SOURCE
#include "win32.h"
#include "d3d9.h"
#include "movie.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* ---------------------------------------------------------------- helpers */
static void ret_ok(void) { RET32(0); }

/* S_OK, and clear a 32-bit out parameter in the first argument slot */
static void ret_ok_out1(void)
{
    uint32_t p = arg32(1);
    if (p)
        M32(p) = 0;
    RET32(0);
}

static const uint8_t IID_IUnknown[16] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 };
static const uint8_t IID_IDispatch[16] = { 0x00, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 };
static const uint8_t IID_IGraphBuilder[16] = { 0xa9, 0x68, 0xa8, 0x56, 0xd4, 0x0a, 0xce, 0x11, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 };
static const uint8_t IID_IMediaControl[16] = { 0xb1, 0x68, 0xa8, 0x56, 0xd4, 0x0a, 0xce, 0x11, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 };
static const uint8_t IID_IMediaEvent[16] = { 0xb6, 0x68, 0xa8, 0x56, 0xd4, 0x0a, 0xce, 0x11, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 };
static const uint8_t IID_IMediaEventEx[16] = { 0xc0, 0x68, 0xa8, 0x56, 0xd4, 0x0a, 0xce, 0x11, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 };
static const uint8_t IID_IVideoWindow[16] = { 0xb4, 0x68, 0xa8, 0x56, 0xd4, 0x0a, 0xce, 0x11, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 };
static const uint8_t IID_IBasicAudio[16] = { 0xb3, 0x68, 0xa8, 0x56, 0xd4, 0x0a, 0xce, 0x11, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 };
static const uint8_t CLSID_FilterGraph[16] = { 0xb3, 0xeb, 0x36, 0xe4, 0x4f, 0x52, 0xce, 0x11, 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 };

/* E_FAIL with a cleared out pointer: the game keeps going without the video */
static void ret_fail(void)
{
    uint32_t p = arg32(1);
    if (p)
        M32(p) = 0;
    RET32(0x80004005u);
}

static const uint8_t IID_IBaseFilter[16] = { 0x95, 0x68, 0xa8, 0x56, 0xd4, 0x0a, 0xce, 0x11, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 };
static const uint8_t IID_IPin[16] = { 0x91, 0x68, 0xa8, 0x56, 0xd4, 0x0a, 0xce, 0x11, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 };
static const uint8_t IID_IMediaFilter[16] = { 0x99, 0x68, 0xa8, 0x56, 0xd4, 0x0a, 0xce, 0x11, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 };

static const uint8_t IID_IMediaPosition[16] = { 0xb2, 0x68, 0xa8, 0x56, 0xd4, 0x0a, 0xce, 0x11, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 };

static const uint8_t IID_IMediaEventSink[16] = { 0xa2, 0x68, 0xa8, 0x56, 0xd4, 0x0a, 0xce, 0x11, 0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 };

static int guid_eq(uint32_t ptr, const uint8_t *want)
{
    if (!ptr)
        return 0;
    const uint8_t *g = MEM + ptr;
    for (int i = 0; i < 16; i++) {
        if (g[i] != want[i])
            return 0;
    }
    return 1;
}

#define IID_ARG(d1, d2, d3, d4, d5) ((uint32_t)(d1)), ((uint16_t)(d2)), \
    ((uint16_t)(d3)), ((uint32_t)(d4)), ((uint32_t)(d5))

/* ------------------------------------------------------------- interfaces */
static uint32_t g_graph, g_control, g_event, g_eventex, g_video, g_audio;
static uint32_t g_filter, g_pin, g_position, g_sink;
static int g_state;             /* 0 stopped, 1 paused, 2 running */
static char g_movie_path[4096]; /* the file the game asked to play (guest) */
static char g_movie_host[4096]; /* the same file as a host path */

uint32_t dshow_sink(void);
#define MAX_GRAPH_FILTERS 8
static uint32_t g_filters[MAX_GRAPH_FILTERS];
static unsigned g_nfilters;

/* Ask the filters the game added to the graph to start (this is what makes
 * its own "TextureRenderer" create the video surface). */
static void call_filters(int vtable_index, int with_args)
{
    for (unsigned i = 0; i < g_nfilters; i++) {
        uint32_t f = g_filters[i];
        if (!f)
            continue;
        uint32_t fn = M32(M32(f) + (uint32_t)vtable_index * 4u);
        if (!fn)
            continue;
        uint32_t r = x86_call_stdcall(fn, &f, 1);
        log_printf("dshow: filter %08x method %d -> %08x gives %08x", f, vtable_index,
                   fn, r);
    }
    (void)with_args;
}

uint32_t dshow_position(void);

uint32_t dshow_filter(void);
uint32_t dshow_pin(void);
int dshow_is_known_iid(uint32_t iid);

static void mc_GetState(void)
{
    uint32_t p = arg32(2);
    if (p)
        M32(p) = (uint32_t)g_state;
    RET32(0);
}

/* The renderer normally creates its target texture from the first video
 * sample.  We do not decode WMV, so create the same texture it would have
 * created (1280x1024 X8R8G8B8) on the game's own device. */
static void ensure_video_texture(uint32_t filter)
{
    uint32_t video = filter - 0xc;
    if (M32(video + 0x160))
        return;
    uint32_t device = M32(0x5e1f20);        /* the game's IDirect3DDevice9 */
    if (!device)
        return;
    uint32_t fn = M32(M32(device) + 23 * 4); /* IDirect3DDevice9::CreateTexture */
    uint32_t args[9];
    args[0] = device;
    args[1] = 1280;                          /* Width  */
    args[2] = 1024;                          /* Height */
    args[3] = 1;                             /* Levels */
    args[4] = 0;                             /* Usage  */
    args[5] = 0x16;                          /* D3DFMT_X8R8G8B8 */
    args[6] = 0;                             /* D3DPOOL_DEFAULT */
    args[7] = video + 0x160;                 /* ppTexture */
    args[8] = 0;                             /* pSharedHandle */
    uint32_t r = x86_call_stdcall(fn, args, 9);
    log_printf("dshow: CreateTexture(%08x) -> %08x texture %08x", fn, r,
               M32(video + 0x160));
}

static void mc_Run(void)
{
    g_state = 2;                    /* State_Running */
    if (g_nfilters)
        ensure_video_texture(g_filters[0]);
    /* Real frames: ffmpeg decodes the WMV3 file into the renderer texture.
     * The movie manager only counts frames, so the texture upload is driven
     * from dshow_movie_tick() once per presented frame.  The startup probe
     * (0x417480) drives the same graph for a few milliseconds just to check
     * DirectShow; decoding there would flash a frame and a blip of sound, so
     * only start the decoder when the movie manager owns slot 0x5DE258. */
    if (g_movie_host[0] && !movie_active()) {
        uint32_t owner = M32(0x5DE258u);
        if (!owner || M32(owner) != 0x55E904u) {
            log_printf("dshow: not the movie manager, skipping the decoder");
            goto no_movie_decoder;
        }
        const char *video_size = getenv("OTOMEDIUS_MOVIE_SIZE");
        int mw = 640, mh = 512;
        if (video_size && sscanf(video_size, "%dx%d", &mw, &mh) != 2) {
            mw = 640;
            mh = 512;
        }
        if (movie_open(g_movie_host, mw, mh, 30) != 0)
            log_printf("dshow: movie decoder failed to start");
    }
no_movie_decoder:
    /* DirectShow: SetSyncSource(NULL), then Pause, then Run */
    for (unsigned i = 0; i < g_nfilters; i++) {
        uint32_t f = g_filters[i];
        uint32_t sa[2];
        sa[0] = f;
        sa[1] = 0;
        x86_call_stdcall(M32(M32(f) + 8 * 4), sa, 2);   /* SetSyncSource(NULL) */
    }
    call_filters(5, 1);         /* IMediaFilter::Pause */
    call_filters(6, 1);         /* IMediaFilter::Run */
    RET32(0);
}

static void mc_Pause(void)
{
    g_state = 1;
    call_filters(5, 1);         /* IMediaFilter::Pause */
    RET32(0);
}

static void mc_Stop(void)
{
    g_state = 0;
    movie_close();
    call_filters(4, 1);         /* IMediaFilter::Stop */
    RET32(0);
}
static void mc_StopWhenReady(void) { RET32(0); }

static void me_GetEventHandle(void)
{
    uint32_t p = arg32(1);
    if (p)
        M32(p) = 0x5151;
    RET32(0);
}

static void me_GetEvent(void)
{
    uint32_t code = arg32(1), p1 = arg32(2), p2 = arg32(3);
    g_state = 0;                    /* the movie is over: the graph stops */
    if (code)
        M32(code) = 1;              /* EC_COMPLETE */
    if (p1)
        M32(p1) = 0;
    if (p2)
        M32(p2) = 0;
    RET32(0);
}

static void me_WaitForCompletion(void)
{
    uint32_t p = arg32(2);
    g_state = 0;
    if (p)
        M32(p) = 1;                 /* EC_COMPLETE: the movie is over */
    RET32(0);
}

static void mx_SetNotifyWindow(void) { RET32(0); }
static void mx_SetNotifyFlags(void) { RET32(0); }
static void mx_GetNotifyFlags(void) { RET32(0); }

/* QueryInterface must be shared by every object: resolve the IID. */
static uint32_t dshow_object_for(uint32_t iid);
uint32_t dshow_graph(void);
uint32_t dshow_control(void);
uint32_t dshow_event(void);
uint32_t dshow_eventex(void);
uint32_t dshow_video(void);
uint32_t dshow_audio(void);

static void ds_QueryInterface(void)
{
    uint32_t iid = arg32(1), ppv = arg32(2);
    uint32_t obj = dshow_object_for(iid);
    if (ppv)
        M32(ppv) = obj;
    RET32(obj ? 0 : 0x80004002u);   /* E_NOINTERFACE */
}

static void ds_AddRef(void) { RET32(2); }
static void ds_Release(void) { RET32(1); }

static void fg_AddFilter(void);
/* Build a VIDEOINFOHEADER media type the way the game's TextureRenderer
 * expects it.  0x4176E0 (its SetMediaType) rejects anything whose formattype
 * is not FORMAT_VideoInfo, whose majortype is not MEDIATYPE_Video, or whose
 * subtype is not MEDIASUBTYPE_RGB24 - it only ever handled the 24-bit BGR
 * frames its WMV decoder produced.  The renderer then creates a 1280x1024
 * X8R8G8B8 texture (we fill that texture with decoded BGRA frames, so the
 * bit depth of the negotiated type does not affect the picture). */
static uint32_t build_video_mediatype(uint32_t subtype)
{
    /* GUIDs in memory order */
    static const uint8_t MEDIATYPE_Video[16] = { 0x76,0x69,0x64,0x73,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71 };
    static const uint8_t FORMAT_VideoInfo[16] = { 0x80,0x9f,0x58,0x05,0x56,0xc3,0xce,0x11,0xbf,0x01,0,0xaa,0,0x55,0x59,0x5a };
    static const uint8_t RGB24[16]  = { 0x7d,0xeb,0x36,0xe4,0x4f,0x52,0xce,0x11,0x9f,0x53,0,0x20,0xaf,0x0b,0xa7,0x70 };
    uint32_t mt = emu_alloc(80);
    uint32_t vih = emu_alloc(128);
    (void)subtype;
    memcpy(MEM + mt, MEDIATYPE_Video, 16);
    memcpy(MEM + mt + 16, RGB24, 16);
    M32(mt + 32) = 1;                       /* bFixedSizeSamples */
    M32(mt + 36) = 0;                       /* bTemporalCompression */
    M32(mt + 40) = 1280u * 1024u * 3u;      /* lSampleSize */
    memcpy(MEM + mt + 44, FORMAT_VideoInfo, 16);
    M32(mt + 60) = 0;                       /* pUnk */
    M32(mt + 64) = 88;                      /* cbFormat */
    M32(mt + 68) = vih;
    /* VIDEOINFOHEADER */
    M32(vih + 0) = 0; M32(vih + 4) = 0; M32(vih + 8) = 1280; M32(vih + 12) = 1024;
    M32(vih + 16) = 0; M32(vih + 20) = 0; M32(vih + 24) = 1280; M32(vih + 28) = 1024;
    M32(vih + 32) = 1280u * 1024u * 3u * 30u;  /* dwBitRate */
    M32(vih + 36) = 0;
    MF64(vih + 40) = 333667.0;              /* AvgTimePerFrame: 30 Hz */
    /* BITMAPINFOHEADER */
    M32(vih + 48) = 40;
    M32(vih + 52) = 1280;
    M32(vih + 56) = 1024;
    M16(vih + 60) = 1;                      /* biPlanes */
    M16(vih + 62) = 24;                     /* biBitCount */
    M32(vih + 64) = 0;                      /* biCompression = BI_RGB */
    M32(vih + 68) = 1280u * 1024u * 3u;     /* biSizeImage */
    return mt;
}

/* IGraphBuilder::Render: connect the source pin to the input pin of the
 * renderer filter the game added to the graph.  That connection is what makes
 * its "TextureRenderer" allocate the video surface. */
static void gb_Render(void)
{
    uint32_t src_pin = arg32(1);
    if (!g_nfilters) {
        RET32(0x80004005u);
        return;
    }
    uint32_t filter = g_filters[0];
    uint32_t out = emu_alloc(16);
    uint32_t args[2];
    M32(out) = 0;
    args[0] = filter;
    args[1] = out;
    uint32_t r = x86_call_stdcall(M32(M32(filter) + 10 * 4), args, 2);  /* EnumPins */
    uint32_t pins = M32(out);
    if ((int32_t)r < 0 || !pins) {
        RET32(0x80004005u);
        return;
    }
    uint32_t nargs[4];
    M32(out) = 0;
    nargs[0] = pins;
    nargs[1] = 1;
    nargs[2] = out;
    nargs[3] = 0;
    r = x86_call_stdcall(M32(M32(pins) + 3 * 4), nargs, 4);             /* Next */
    uint32_t in_pin = M32(out);
    if ((int32_t)r < 0 || !in_pin) {
        RET32(0x80004005u);
        return;
    }
    for (int sub = 1; sub >= 0; sub--) {
        uint32_t mt = build_video_mediatype((uint32_t)sub);
        uint32_t rca[3];
        rca[0] = in_pin;
        rca[1] = src_pin;
        rca[2] = mt;
        if (getenv("OTOMEDIUS_MOVIE_DBG"))
            fprintf(stderr, "dshow: Render: pin=%08x vtbl=%08x method=%08x\n",
                    in_pin, M32(in_pin), M32(M32(in_pin) + 4 * 4));
        r = x86_call_stdcall(M32(M32(in_pin) + 4 * 4), rca, 3);         /* ReceiveConnection */
        log_printf("dshow: Render: subtype %d -> ReceiveConnection %08x", sub, r);
        if ((int32_t)r >= 0) {
            RET32(0);
            return;
        }
    }
    RET32(0x80004005u);
}

static void gb_AddSourceFilter(void);

#define TABLE(name, list) static const method_t name[] = { list };

static const method_t graph_vtbl[] = {
    {ds_QueryInterface, 3}, {ds_AddRef, 1}, {ds_Release, 1},
    {fg_AddFilter, 3}, {ret_ok, 2}, {ret_fail, 2}, {ret_fail, 3}, {ret_ok, 4},
    {ret_ok, 2}, {ret_ok, 2}, {ret_ok, 1},
    {ret_ok, 3}, {gb_Render, 2}, {ret_ok, 3}, {gb_AddSourceFilter, 4}, {ret_ok, 2},
    {ret_ok, 1}, {ret_ok, 1},
};

static const method_t control_vtbl[] = {
    {ds_QueryInterface, 3}, {ds_AddRef, 1}, {ds_Release, 1},
    {ret_ok, 2}, {ret_ok, 4}, {ret_ok, 6}, {ret_ok, 9},
    {mc_Run, 1}, {mc_Pause, 1}, {mc_Stop, 1}, {mc_GetState, 3},
    {ret_ok, 2}, {ret_fail, 3}, {ret_fail, 2}, {ret_fail, 2}, {mc_StopWhenReady, 1},
};

static const method_t event_vtbl[] = {
    {ds_QueryInterface, 3}, {ds_AddRef, 1}, {ds_Release, 1},
    {ret_ok, 2}, {ret_ok, 4}, {ret_ok, 6}, {ret_ok, 9},
    {me_GetEventHandle, 2}, {me_GetEvent, 5}, {me_WaitForCompletion, 3},
    {ret_ok, 2}, {ret_ok, 2}, {ret_ok, 4},
};

static const method_t eventex_vtbl[] = {
    {ds_QueryInterface, 3}, {ds_AddRef, 1}, {ds_Release, 1},
    {ret_ok, 2}, {ret_ok, 4}, {ret_ok, 6}, {ret_ok, 9},
    {me_GetEventHandle, 2}, {me_GetEvent, 5}, {me_WaitForCompletion, 3},
    {ret_ok, 2}, {ret_ok, 2}, {ret_ok, 4},
    {mx_SetNotifyWindow, 4}, {mx_SetNotifyFlags, 2}, {mx_GetNotifyFlags, 2},
};

/* IVideoWindow: IDispatch + 39 methods, all accepted */
static const method_t video_vtbl[] = {
    {ds_QueryInterface, 3}, {ds_AddRef, 1}, {ds_Release, 1},
    {ret_ok, 2}, {ret_ok, 4}, {ret_ok, 6}, {ret_ok, 9},
    {ret_ok, 2}, {ret_ok_out1, 2}, {ret_ok, 2}, {ret_ok_out1, 2},
    {ret_ok, 2}, {ret_ok_out1, 2}, {ret_ok, 2}, {ret_ok_out1, 2},
    {ret_ok, 2}, {ret_ok_out1, 2}, {ret_ok, 2}, {ret_ok_out1, 2},
    {ret_ok, 2}, {ret_ok_out1, 2}, {ret_ok, 2}, {ret_ok_out1, 2},
    {ret_ok, 2}, {ret_ok_out1, 2}, {ret_ok, 2}, {ret_ok_out1, 2},
    {ret_ok, 2}, {ret_ok_out1, 2}, {ret_ok, 2}, {ret_ok_out1, 2},
    {ret_ok, 2}, {ret_ok_out1, 2}, {ret_ok, 2}, {ret_ok_out1, 2},
    {ret_ok, 2}, {ret_ok_out1, 2}, {ret_ok, 2}, {ret_ok, 5},
    {ret_ok, 5}, {ret_ok, 5}, {ret_ok, 3}, {ret_ok, 3}, {ret_ok, 5},
    {ret_ok, 2}, {ret_ok_out1, 2},
};

static const method_t audio_vtbl[] = {
    {ds_QueryInterface, 3}, {ds_AddRef, 1}, {ds_Release, 1},
    {ret_ok, 2}, {ret_ok, 4}, {ret_ok, 6}, {ret_ok, 9},
    {ret_ok, 2}, {ret_ok_out1, 2}, {ret_ok, 2}, {ret_ok_out1, 2},
};

static void fg_AddFilter(void)
{
    uint32_t f = arg32(1), name = arg32(2), graph = arg32(0);
    /* The game builds a fresh graph for every movie (the startup probe and the
     * movie manager both do AddFilter(renderer) + AddSourceFilter).  Clear the
     * previous filter list when a new graph is being assembled, otherwise the
     * freed renderer filter from the previous graph is still in the list. */
    if (g_state == 0)
        g_nfilters = 0;
    if (g_nfilters < MAX_GRAPH_FILTERS) {
        g_filters[g_nfilters++] = f;
        log_printf("dshow: AddFilter(%08x)", f);
    }
    /* DirectShow tells the filter which graph it belongs to */
    uint32_t jargs[3];
    jargs[0] = f;
    jargs[1] = graph;
    jargs[2] = name;
    uint32_t r = x86_call_stdcall(M32(M32(f) + 13 * 4), jargs, 3);  /* JoinFilterGraph */
    log_printf("dshow: JoinFilterGraph -> %08x", r);
    RET32(0);
}

static void gb_AddSourceFilter(void)
{
    uint32_t pp = arg32(3);
    uint32_t fail = 0;
    const char *env = getenv("OTOMEDIUS_NOMOVIE");
    /* AddSourceFilter(lpstrFileName, lpstrFilterName, ppFilter): the game
     * converts ".\Data\OTOMEDIUS_OP_f.wmv" to UTF-16 before COM sees it. */
    {
        const char *file = getenv("OTOMEDIUS_MOVIE_FILE");
        uint32_t src = arg32(1);
        if (file && *file) {
            /* diagnostic override: already a host path */
            snprintf(g_movie_path, sizeof(g_movie_path), "%s", file);
            snprintf(g_movie_host, sizeof(g_movie_host), "%s", file);
        } else {
            size_t i;
            char *host;
            for (i = 0; src && i + 1 < sizeof(g_movie_path); i++) {
                uint16_t c = M16(src + (uint32_t)i * 2u);
                if (!c)
                    break;
                g_movie_path[i] = (char)c;
            }
            g_movie_path[i] = 0;
            host = win_host_path(g_movie_path);
            if (host) {
                snprintf(g_movie_host, sizeof(g_movie_host), "%s", host);
                free(host);
            }
        }
        log_printf("dshow: source file \"%s\" -> \"%s\"", g_movie_path,
                   g_movie_host);
    }
    if (env && *env && strcmp(env, "0") != 0)
        fail = 1;                       /* diagnostic: pretend the file is missing */
    if (pp)
        M32(pp) = fail ? 0 : dshow_filter();
    if (fail) {
        log_printf("dshow: AddSourceFilter -> VFW_E_NOT_FOUND (diagnostic)");
        RET32(0x80040216u);             /* VFW_E_NOT_FOUND */
        return;
    }
    log_printf("dshow: AddSourceFilter -> filter %08x", M32(pp));
    RET32(0);
}

/* ------------------------------------------------- filter and pin (video) */
static uint32_t pin_id_string(void)
{
    static uint32_t str;
    if (!str) {
        str = emu_alloc(32);
        M16(str + 0) = 'O';
        M16(str + 2) = 'u';
        M16(str + 4) = 't';
        M16(str + 6) = 'p';
        M16(str + 8) = 'u';
        M16(str + 10) = 't';
        M16(str + 12) = 0;
    }
    return str;
}

static void pin_QueryId(void)
{
    uint32_t p = arg32(1);
    if (p)
        M32(p) = pin_id_string();
    RET32(0);
}

static void pin_QueryDirection(void)
{
    uint32_t p = arg32(1);
    if (p)
        M32(p) = 1;                 /* PINDIR_OUTPUT */
    RET32(0);
}

static void pin_QueryPinInfo(void)
{
    uint32_t info = arg32(1);
    if (info) {
        M32(info) = dshow_filter();
        M32(info + 4) = 1;          /* PINDIR_OUTPUT */
        M16(info + 8) = 'O';
        M16(info + 10) = 0;
    }
    RET32(0);
}

static void pin_ConnectedTo(void)
{
    uint32_t p = arg32(1);
    if (p)
        M32(p) = 0;
    RET32(1);                       /* S_FALSE: not connected */
}

static void filter_FindPin(void)
{
    uint32_t pp = arg32(2);
    if (pp)
        M32(pp) = dshow_pin();
    RET32(0);
}

static void filter_QueryFilterInfo(void)
{
    uint32_t info = arg32(1);
    if (info) {
        M32(info) = dshow_graph();
        M16(info + 4) = 'O';
        M16(info + 6) = 0;
    }
    RET32(0);
}

static void filter_EnumPins(void)
{
    uint32_t pp = arg32(1);
    if (pp)
        M32(pp) = 0;
    RET32(0x80004001u);             /* E_NOTIMPL */
}

static void mp_get_Duration(void)
{
    uint32_t p = arg32(1);
    if (p) {
        MF64(p) = 1.0;          /* the movie is one second long and over */
    }
    RET32(0);
}

static void mp_get_CurrentPosition(void)
{
    uint32_t p = arg32(1);
    if (p)
        MF64(p) = 1.0;
    RET32(0);
}

static void mp_CanSeek(void)
{
    uint32_t p = arg32(1);
    if (p)
        M32(p) = 0;             /* cannot seek: nothing to play */
    RET32(0);
}

/* IMediaEventSink: the filter reports events to the graph through this. */
static void sk_Notify(void)
{
    uint32_t code = arg32(1), p1 = arg32(2), p2 = arg32(3);
    log_printf("dshow: filter event %u (p1=%08x p2=%08x)", code, p1, p2);
    RET32(0);
}

static const method_t sink_vtbl[] = {
    {ds_QueryInterface, 3}, {ds_AddRef, 1}, {ds_Release, 1}, {sk_Notify, 4},
};

static const method_t position_vtbl[] = {
    {ds_QueryInterface, 3}, {ds_AddRef, 1}, {ds_Release, 1},
    {ret_ok, 2}, {ret_ok, 4}, {ret_ok, 6}, {ret_ok, 9},
    {mp_get_Duration, 2}, {ret_ok, 2}, {mp_get_CurrentPosition, 2},
    {mp_get_Duration, 2}, {ret_ok, 2}, {mp_CanSeek, 2}, {ret_ok, 2},
    {ret_ok, 2}, {mp_CanSeek, 2}, {mp_CanSeek, 2}, {mp_CanSeek, 2},
};

static const method_t filter_vtbl[] = {
    {ds_QueryInterface, 3}, {ds_AddRef, 1}, {ds_Release, 1},
    {ret_ok, 2},                                   /* GetClassID */
    {ret_ok, 1}, {ret_ok, 1}, {ret_ok, 1},         /* Stop Pause Run */
    {mc_GetState, 3},                              /* GetState */
    {ret_ok, 2}, {ret_ok_out1, 2},                 /* Set/GetSyncSource */
    {filter_EnumPins, 2}, {filter_FindPin, 3},
    {filter_QueryFilterInfo, 2}, {ret_ok, 3},      /* JoinFilterGraph */
    {ret_ok, 2},                                   /* QueryVendorInfo */
};

static const method_t pin_vtbl[] = {
    {ds_QueryInterface, 3}, {ds_AddRef, 1}, {ds_Release, 1},
    {ret_ok, 3}, {ret_ok, 3},                      /* Connect ReceiveConnection */
    {ret_ok, 1},                                   /* Disconnect */
    {pin_ConnectedTo, 2}, {ret_ok, 2},             /* ConnectionMediaType */
    {pin_QueryPinInfo, 2}, {pin_QueryDirection, 2}, {pin_QueryId, 2},
    {ret_ok, 2}, {ret_fail, 2},                    /* QueryAccept EnumMediaTypes */
    {ret_ok, 3},                                   /* QueryInternalConnections */
    {ret_ok, 1}, {ret_ok, 1}, {ret_ok, 1},         /* EndOfStream BeginFlush EndFlush */
    {ret_ok, 5},                                   /* NewSegment */
};

uint32_t dshow_filter(void)
{
    if (!g_filter)
        g_filter = com_create_named("IBaseFilter", filter_vtbl,
                              (int)(sizeof(filter_vtbl) / sizeof(method_t)), 8);
    return g_filter;
}

uint32_t dshow_sink(void)
{
    if (!g_sink)
        g_sink = com_create_named("IMediaEventSink", sink_vtbl,
                                  (int)(sizeof(sink_vtbl) / sizeof(method_t)), 8);
    return g_sink;
}

uint32_t dshow_position(void)
{
    if (!g_position)
        g_position = com_create_named("IMediaPosition", position_vtbl,
                                (int)(sizeof(position_vtbl) / sizeof(method_t)), 8);
    return g_position;
}

uint32_t dshow_pin(void)
{
    if (!g_pin)
        g_pin = com_create_named("IPin", pin_vtbl, (int)(sizeof(pin_vtbl) / sizeof(method_t)), 8);
    return g_pin;
}

#define MAKE(obj, table, name)                                               \
    do {                                                                     \
        if (!(obj))                                                          \
            (obj) = com_create_named((name), (table),                        \
                                     (int)(sizeof(table) / sizeof(method_t)), 8); \
    } while (0)

uint32_t dshow_graph(void)    { MAKE(g_graph, graph_vtbl, "IGraphBuilder"); return g_graph; }
uint32_t dshow_control(void)  { MAKE(g_control, control_vtbl, "IMediaControl"); return g_control; }
uint32_t dshow_event(void)    { MAKE(g_event, event_vtbl, "IMediaEvent"); return g_event; }
uint32_t dshow_eventex(void)  { MAKE(g_eventex, eventex_vtbl, "IMediaEventEx"); return g_eventex; }
uint32_t dshow_video(void)    { MAKE(g_video, video_vtbl, "IVideoWindow"); return g_video; }
uint32_t dshow_audio(void)    { MAKE(g_audio, audio_vtbl, "IBasicAudio"); return g_audio; }

/* IID 24 char hex helpers */
static uint32_t dshow_object_for(uint32_t iid)
{
    if (!iid)
        return 0;
    if (guid_eq(iid, IID_IUnknown) || guid_eq(iid, IID_IGraphBuilder))
        return dshow_graph();
    if (guid_eq(iid, IID_IMediaControl) || guid_eq(iid, IID_IDispatch))
        return dshow_control();
    if (guid_eq(iid, IID_IMediaEvent))
        return dshow_event();
    if (guid_eq(iid, IID_IMediaEventEx))
        return dshow_eventex();
    if (guid_eq(iid, IID_IVideoWindow))
        return dshow_video();
    if (guid_eq(iid, IID_IBasicAudio))
        return dshow_audio();
    if (guid_eq(iid, IID_IBaseFilter) || guid_eq(iid, IID_IMediaFilter))
        return dshow_filter();
    if (guid_eq(iid, IID_IPin))
        return dshow_pin();
    if (guid_eq(iid, IID_IMediaPosition))
        return dshow_position();
    if (guid_eq(iid, IID_IMediaEventSink))
        return dshow_sink();
    log_printf("dshow: QueryInterface for {%08x-%04x-%04x} not implemented",
               M32(iid), M16(iid + 4), M16(iid + 6));
    return 0;
}

/* Called once per presented frame: copy the newest decoded video frame into
 * the texture the game's own TextureRenderer created ([wrapper+0x10]+0x160).
 * The movie manager's draw callback (0x43F540) samples that texture. */
void dshow_movie_tick(void)
{
    uint32_t wrap, filter, tex, tw = 0, th = 0;
    const uint32_t *px;
    static unsigned uploaded;
    int got;

    if (!movie_active())
        return;
    wrap = M32(0x5DCF88u);
    if (!wrap)
        return;
    filter = M32(wrap + 0x10u);
    if (!filter)
        return;
    tex = M32(filter + 0x160u);
    if (!tex)
        return;
    px = texture_argb(tex, &tw, &th);
    if (!px || !tw || !th)
        return;
    got = movie_frame((uint8_t *)(uintptr_t)px, (int)tw, (int)th);
    if (got)
        uploaded++;
    if (getenv("OTOMEDIUS_MOVIE_DBG") && got &&
        (uploaded == 1 || uploaded % 120 == 0))
        fprintf(stderr, "movie: frame %u uploaded to tex %08x (%ux%u)\n",
                uploaded, tex, tw, th);
}

/* CLSID_FilterGraph {e436ebb3-524f-11ce-9f53-0020af0ba770} */
int dshow_create_filter_graph(uint32_t rclsid, uint32_t riid, uint32_t ppv)
{
    if (!rclsid)
        return 0;
    if (!guid_eq(rclsid, CLSID_FilterGraph))
        return 0;
    uint32_t obj = dshow_object_for(riid);
    if (ppv)
        M32(ppv) = obj;
    log_printf("dshow: filter graph created (%08x)", obj);
    return obj ? 1 : 2;   /* 2 = class found but interface missing */
}
