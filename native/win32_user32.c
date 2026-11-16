/* win32_user32.c - minimal window manager: one window, message queue. */

#define _GNU_SOURCE
#include "win32.h"

#include <string.h>
#include <stdlib.h>

#define HWND_MAIN 0x00010001u

typedef struct {
    uint32_t hwnd;
    uint32_t msg;
    uint32_t wparam;
    uint32_t lparam;
} queued_msg_t;

static queued_msg_t g_queue[64];
static int g_qhead, g_qtail;
static uint32_t g_wndproc;
static uint32_t g_hinstance;
static int g_quit;
static uint32_t g_cursor;

static void push_msg(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);

void win_request_quit(void)
{
    g_quit = 1;
    /* always deliver WM_QUIT: this is what makes the window close button and
     * Esc end the game */
    push_msg(HWND_MAIN, 0x0012, 0, 0);
}

static void push_msg(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    int next = (g_qtail + 1) & 63;
    if (next == g_qhead)
        return;
    g_queue[g_qtail].hwnd = hwnd;
    g_queue[g_qtail].msg = msg;
    g_queue[g_qtail].wparam = wp;
    g_queue[g_qtail].lparam = lp;
    g_qtail = next;
}

/* Call the game's window procedure with a message. */
static uint32_t call_wndproc(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    if (!g_wndproc)
        return 0;
    cpu_t *c = x86_cpu();
    uint32_t saved_esp = c->esp;
    x86_push32(c, lp);
    x86_push32(c, wp);
    x86_push32(c, msg);
    x86_push32(c, hwnd);
    x86_push32(c, X86_RET_SENTINEL); /* the procedure returns to us */
    x86_dispatch(g_wndproc);
    x86_run();
    c->esp = saved_esp;
    return c->eax;
}

/* --------------------------------------------------------------- exports */
static void u_RegisterClassExA(void)
{
    uint32_t p = arg32(0);
    if (p)
        g_wndproc = M32(p + 8);      /* lpfnWndProc */
    RET32(0xC000);
}

static void u_RegisterClassA(void)
{
    uint32_t p = arg32(0);
    if (p)
        g_wndproc = M32(p + 4);      /* WNDCLASSA: style, lpfnWndProc */
    RET32(0xC000);
}

static void u_UnregisterClassA(void) { RET32(1); }

static void u_CreateWindowExA(void)
{
    const char *name = arg_str(1);
    int w = (int)arg32(6), h = (int)arg32(7);
    (void)name;
    log_printf("user32: CreateWindowExA(%dx%d) wndproc=%08x", w, h, g_wndproc);
    call_wndproc(HWND_MAIN, 0x0001, 0, 0);   /* WM_CREATE */
    /* a real window manager would follow up with these */
    call_wndproc(HWND_MAIN, 0x0005, 0, ((uint32_t)h << 16) | (uint32_t)w);  /* WM_SIZE */
    call_wndproc(HWND_MAIN, 0x0006, 1, 0);   /* WM_ACTIVATE (WA_ACTIVE) */
    call_wndproc(HWND_MAIN, 0x0007, 0, 0);   /* WM_SETFOCUS */
    RET32(HWND_MAIN);
}

static void u_DestroyWindow(void) { RET32(1); }
static void u_ShowWindow(void) { RET32(0); }
static void u_UpdateWindow(void) { RET32(1); }
static void u_DefWindowProcA(void) { RET32(0); }

static void u_PeekMessageA(void)
{
    uint32_t pmsg = arg32(0), remove = arg32(4);
    if (g_qhead == g_qtail) {
        RET32(0);
        return;
    }
    queued_msg_t *m = &g_queue[g_qhead];
    if (remove)
        g_qhead = (g_qhead + 1) & 63;
    if (pmsg) {
        M32(pmsg + 0) = m->hwnd;
        M32(pmsg + 4) = m->msg;
        M32(pmsg + 8) = m->wparam;
        M32(pmsg + 12) = m->lparam;
        M32(pmsg + 16) = 0;
        M32(pmsg + 20) = 0;
    }
    RET32(1);
}

static void u_GetMessageA(void)
{
    uint32_t pmsg = arg32(0);
    if (g_qhead == g_qtail) {
        RET32(0);
        return;
    }
    if (pmsg) {
        queued_msg_t *m = &g_queue[g_qhead];
        M32(pmsg + 0) = m->hwnd;
        M32(pmsg + 4) = m->msg;
        M32(pmsg + 8) = m->wparam;
        M32(pmsg + 12) = m->lparam;
        M32(pmsg + 16) = 0;
        M32(pmsg + 20) = 0;
        g_qhead = (g_qhead + 1) & 63;
    }
    RET32(1);
}

static void u_TranslateMessage(void) { RET32(0); }

static void u_DispatchMessageA(void)
{
    uint32_t pmsg = arg32(0);
    if (pmsg)
        RET32(call_wndproc(M32(pmsg), M32(pmsg + 4), M32(pmsg + 8), M32(pmsg + 12)));
    else
        RET32(0);
}

static void u_PostQuitMessage(void)
{
    win_request_quit();
}

static void u_PostMessageA(void)
{
    push_msg(arg32(0), arg32(1), arg32(2), arg32(3));
    RET32(1);
}

static void u_PostThreadMessageA(void) { RET32(1); }
static void u_SendMessageA(void) { RET32(0); }

static void u_MsgWaitForMultipleObjects(void)
{
    extern void platform_wait(void);
    platform_wait();
    RET32(0x00000102u);  /* WAIT_TIMEOUT: no native window events yet */
}

static void u_GetQueueStatus(void) { RET32(0); }
static void u_RegisterWindowMessageA(void) { RET32(0xC100); }

static void u_LoadCursorA(void) { RET32(0xC200); }
static void u_LoadIconA(void) { RET32(0xC201); }
static void u_LoadImageA(void) { RET32(0xC202); }
static void u_SetCursor(void) { RET32(1); }
static void u_GetDC(void) { RET32(0x0DC00001u); }
static void u_ReleaseDC(void) { RET32(1); }
static void u_GetActiveWindow(void) { RET32(HWND_MAIN); }
static void u_ClientToScreen(void) { RET32(1); }
static void u_AdjustWindowRect(void) { RET32(1); }
static void u_DestroyMenu(void) { RET32(1); }
static void u_LoadMenuA(void) { RET32(0); }

const import_def_t user32_imports[] = {
    {"user32.dll", "SetCursor", u_SetCursor, 1},
    {"user32.dll", "LoadImageA", u_LoadImageA, 6},
    {"user32.dll", "DefWindowProcA", u_DefWindowProcA, 4},
    {"user32.dll", "ClientToScreen", u_ClientToScreen, 2},
    {"user32.dll", "PostMessageA", u_PostMessageA, 4},
    {"user32.dll", "PostQuitMessage", u_PostQuitMessage, 1},
    {"user32.dll", "LoadIconA", u_LoadIconA, 2},
    {"user32.dll", "MsgWaitForMultipleObjects", u_MsgWaitForMultipleObjects, 5},
    {"user32.dll", "RegisterWindowMessageA", u_RegisterWindowMessageA, 1},
    {"user32.dll", "GetQueueStatus", u_GetQueueStatus, 1},
    {"user32.dll", "PostThreadMessageA", u_PostThreadMessageA, 4},
    {"user32.dll", "LoadCursorA", u_LoadCursorA, 2},
    {"user32.dll", "ReleaseDC", u_ReleaseDC, 2},
    {"user32.dll", "GetDC", u_GetDC, 1},
    {"user32.dll", "GetActiveWindow", u_GetActiveWindow, 0},
    {"user32.dll", "UnregisterClassA", u_UnregisterClassA, 2},
    {"user32.dll", "DestroyMenu", u_DestroyMenu, 1},
    {"user32.dll", "UpdateWindow", u_UpdateWindow, 1},
    {"user32.dll", "ShowWindow", u_ShowWindow, 2},
    {"user32.dll", "CreateWindowExA", u_CreateWindowExA, 12},
    {"user32.dll", "LoadMenuA", u_LoadMenuA, 2},
    {"user32.dll", "AdjustWindowRect", u_AdjustWindowRect, 3},
    {"user32.dll", "DispatchMessageA", u_DispatchMessageA, 1},
    {"user32.dll", "TranslateMessage", u_TranslateMessage, 1},
    {"user32.dll", "RegisterClassExA", u_RegisterClassExA, 1},
    {"user32.dll", "PeekMessageA", u_PeekMessageA, 5},
    {"user32.dll", "DestroyWindow", u_DestroyWindow, 1},
    {"user32.dll", "GetMessageA", u_GetMessageA, 4},
    {"user32.dll", "SendMessageA", u_SendMessageA, 4},
    {"user32.dll", "RegisterClassA", u_RegisterClassA, 1},
    {NULL, NULL, NULL, 0},
};

void win_post_key(uint32_t vk, int down)
{
    push_msg(HWND_MAIN, down ? 0x0100u : 0x0101u, vk, 1);
}

uint32_t win_main_window(void) { return HWND_MAIN; }
