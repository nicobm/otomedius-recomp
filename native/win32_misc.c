/* win32_misc.c - gdi32, ole32, oleaut32, shlwapi, imm32, winmm, dsound. */

#define _GNU_SOURCE
#include "win32.h"

#include <string.h>
#include <stdlib.h>

static void misc_stub(void) { RET32(0); }

/* -------------------------------------------------------------- gdi32 */
static void g_CreateFontA(void) { RET32(0x0F000001u); }
static void g_DeleteObject(void) { RET32(1); }
static void g_SelectObject(void) { RET32(0x0F000002u); }
static void g_GetGlyphOutlineA(void) { RET32(0xFFFFFFFFu); }   /* GDI_ERROR */
static void g_GetTextMetricsA(void) { RET32(1); }
static void g_GetCharABCWidthsA(void) { RET32(1); }
static void g_AddFontMemResourceEx(void) { RET32(0); }

const import_def_t gdi32_imports[] = {
    {"gdi32.dll", "CreateFontA", g_CreateFontA, 14},
    {"gdi32.dll", "DeleteObject", g_DeleteObject, 1},
    {"gdi32.dll", "GetGlyphOutlineA", g_GetGlyphOutlineA, 7},
    {"gdi32.dll", "GetTextMetricsA", g_GetTextMetricsA, 2},
    {"gdi32.dll", "GetCharABCWidthsA", g_GetCharABCWidthsA, 4},
    {"gdi32.dll", "AddFontMemResourceEx", g_AddFontMemResourceEx, 4},
    {"gdi32.dll", "SelectObject", g_SelectObject, 2},
    {NULL, NULL, NULL, 0},
};

/* ------------------------------------------------------------- ole32 */
static void o_CoTaskMemFree(void) { }
static void o_CoCreateInstance(void)
{
    uint32_t rclsid = arg32(0), riid = arg32(3), ppv = arg32(4);
    int r = dshow_create_filter_graph(rclsid, riid, ppv);
    if (r == 1) { RET32(0); return; }              /* S_OK */
    if (r == 2) { RET32(0x80004002u); return; }    /* E_NOINTERFACE */
    if (rclsid) {
        const uint8_t *c = MEM + rclsid;
        log_printf("ole32: CoCreateInstance {%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x} "
                   "riid {%08x-%04x-...} -> not implemented",
                   M32(rclsid), M16(rclsid + 4), M16(rclsid + 6), c[8], c[9], c[10],
                   c[11], c[12], c[13], c[14], c[15],
                   riid ? M32(riid) : 0, riid ? M16(riid + 4) : 0);
    }
    if (ppv)
        M32(ppv) = 0;
    RET32(0x80040154u);   /* REGDB_E_CLASSNOTREG */
}
static void o_CoInitialize(void) { RET32(0); }
static void o_CoFreeUnusedLibraries(void) { }
static void o_CoTaskMemAlloc(void) { RET32(0); }
static void o_CoUninitialize(void) { }
static void o_SysAllocString(void) { RET32(0); }
static void o_SysFreeString(void) { }

const import_def_t ole_imports[] = {
    {"ole32.dll", "CoTaskMemFree", o_CoTaskMemFree, 1},
    {"ole32.dll", "CoCreateInstance", o_CoCreateInstance, 5},
    {"ole32.dll", "CoInitialize", o_CoInitialize, 1},
    {"ole32.dll", "CoFreeUnusedLibraries", o_CoFreeUnusedLibraries, 0},
    {"ole32.dll", "CoTaskMemAlloc", o_CoTaskMemAlloc, 1},
    {"ole32.dll", "CoUninitialize", o_CoUninitialize, 0},
    {"oleaut32.dll", "ordinal:2", o_SysAllocString, 1},
    {"oleaut32.dll", "ordinal:6", o_SysFreeString, 1},
    {"shlwapi.dll", "PathRelativePathToA", misc_stub, 6},
    {NULL, NULL, NULL, 0},
};

/* ------------------------------------------------------------- winmm */
static void w_timeKillEvent(void) { RET32(0); }
static void w_timeSetEvent(void) { RET32(1); }
static void w_timeBeginPeriod(void) { RET32(0); }
static void w_timeEndPeriod(void) { RET32(0); }
static void w_timeGetTime(void)
{
    extern void win_tick(uint32_t *lo);
    uint32_t v = 0;
    win_tick(&v);
    RET32(v);
}

const import_def_t winmm_imports[] = {
    {"winmm.dll", "timeKillEvent", w_timeKillEvent, 1},
    {"winmm.dll", "timeSetEvent", w_timeSetEvent, 5},
    {"winmm.dll", "timeBeginPeriod", w_timeBeginPeriod, 1},
    {"winmm.dll", "timeGetTime", w_timeGetTime, 0},
    {"winmm.dll", "timeEndPeriod", w_timeEndPeriod, 1},
    {NULL, NULL, NULL, 0},
};

const import_def_t imm32_imports[] = {
    {"imm32.dll", "ImmDisableIME", misc_stub, 1},
    {NULL, NULL, NULL, 0},
};

