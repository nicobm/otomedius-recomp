/* win32_kernel32.c - the kernel32 surface the game and the MSVC CRT use. */

#define _GNU_SOURCE
#include "win32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <iconv.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <sys/mman.h>

/* ------------------------------------------------------------- game paths */
static char g_game_dir[4096] = ".";

const char *win_game_dir(void) { return g_game_dir; }

void win_set_game_dir(const char *dir)
{
    snprintf(g_game_dir, sizeof(g_game_dir), "%s", dir);
}

char *win_host_path(const char *guest)
{
    char tmp[4096];
    const char *src = guest;
    if (!guest)
        return NULL;
    /* "E:\nvmem\x.bin" -> "x.bin"; ".\Data\a.pak" -> "Data/a.pak" */
    if (guest[0] && guest[1] == ':')
        src = guest + 2;
    while (*src == '\\' || *src == '/')
        src++;
    /* The cabinet kept its non-volatile memory in E:\\nvmem (testmode.bin,
     * bookkeep.bin, ea_conf.bin, ...); the dump ships those files in the game
     * directory itself, so drop that component. */
    if ((src[0] | 0x20) == 'n' && (src[1] | 0x20) == 'v' &&
        (src[2] | 0x20) == 'm' && (src[3] | 0x20) == 'e' &&
        (src[4] | 0x20) == 'm' && (src[5] == '\\' || src[5] == '/'))
        src += 6;
    size_t n = 0;
    for (; *src && n + 2 < sizeof(tmp); src++)
        tmp[n++] = (*src == '\\') ? '/' : *src;
    tmp[n] = 0;
    char *out = (char *)malloc(strlen(g_game_dir) + n + 2);
    if (!out)
        return NULL;
    sprintf(out, "%s/%s", g_game_dir, tmp);
    return out;
}

/* -------------------------------------------------------------- handles */
enum { H_FREE = 0, H_FILE, H_EVENT, H_SEM, H_THREAD, H_MAPPING, H_VIEW };

typedef struct {
    int type;
    int fd;
    void *ptr;
    size_t size;
    uint32_t extra;
} handle_t;

/* one emulated thread; lives as long as its handle because WaitForSingleObject
 * needs it after the thread has finished */
typedef struct {
    uint32_t start;
    uint32_t param;
    uint32_t stack_top;
    uint32_t teb;
    volatile int done;
} thread_arg_t;

#define MAX_HANDLES 4096
static handle_t g_handles[MAX_HANDLES];
static pthread_mutex_t g_handles_lock = PTHREAD_MUTEX_INITIALIZER;

#define HANDLE_BASE 0x1000u

static uint32_t handle_new(int type, int fd, void *ptr, size_t size)
{
    pthread_mutex_lock(&g_handles_lock);
    for (int i = 0; i < MAX_HANDLES; i++) {
        if (g_handles[i].type == H_FREE) {
            g_handles[i].type = type;
            g_handles[i].fd = fd;
            g_handles[i].ptr = ptr;
            g_handles[i].size = size;
            g_handles[i].extra = 0;
            pthread_mutex_unlock(&g_handles_lock);
            return HANDLE_BASE + (uint32_t)i * 4u;
        }
    }
    pthread_mutex_unlock(&g_handles_lock);
    return 0;
}

static handle_t *handle_get(uint32_t h)
{
    if (h < HANDLE_BASE)
        return NULL;
    uint32_t i = (h - HANDLE_BASE) / 4u;
    if (i >= MAX_HANDLES || g_handles[i].type == H_FREE)
        return NULL;
    return &g_handles[i];
}

/* ------------------------------------------------------------- allocator */
/* Two arenas inside the emulated space: heap blocks and page allocations. */
#define HEAP_ARENA_BASE  0x10000000u
#define HEAP_ARENA_END   0x30000000u
#define MAP_ARENA_BASE   0x0A000000u
#define MAP_ARENA_END    0x20000000u
#define PAGE_ARENA_BASE  0x30000000u
#define PAGE_ARENA_END   0x60000000u

typedef struct hdr {
    uint32_t size;      /* payload size, 8 byte aligned */
    uint32_t used;
    uint32_t heap;
    uint32_t magic;
} hdr_t;

#define HDR_MAGIC 0x4F544D48u

static uint32_t g_heap_bump = HEAP_ARENA_BASE;
static uint32_t g_page_bump = PAGE_ARENA_BASE;
static pthread_mutex_t g_alloc_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t g_map_bump = MAP_ARENA_BASE;

/* File mapping views live in their own window of the emulated space. */
static uint32_t map_arena_alloc(uint32_t size)
{
    size = (size + 4095u) & ~4095u;
    if (!size)
        size = 4096;
    if (g_map_bump + size > MAP_ARENA_END)
        return 0;
    uint32_t p = g_map_bump;
    g_map_bump += size;
    memset(MEM + p, 0, size);
    return p;
}

/* The scene manager object lives around 0x1003D5F8; if that block is freed
 * and handed out again while still in use its fields get overwritten (we saw
 * the sub-state turn into the text "lv_f").  Log any allocator traffic there. */
#define WATCH_LO 0x1003D000u
#define WATCH_HI 0x1003E000u

static uint32_t heap_alloc(uint32_t heap, uint32_t size)
{
    size = (size + 7u) & ~7u;
    if (!size)
        size = 8;
    pthread_mutex_lock(&g_alloc_lock);
    /* first fit over the arena */
    uint32_t a = HEAP_ARENA_BASE;
    while (a < g_heap_bump) {
        hdr_t *h = (hdr_t *)(MEM + a);
        if (h->magic == HDR_MAGIC && !h->used && h->size >= size) {
            h->used = 1;
            h->heap = heap;
            pthread_mutex_unlock(&g_alloc_lock);
            return a + sizeof(hdr_t);
        }
        a += sizeof(hdr_t) + h->size;
        if (h->magic != HDR_MAGIC)
            break;
    }
    if (g_heap_bump + sizeof(hdr_t) + size > HEAP_ARENA_END) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "kernel32: heap arena exhausted (bump=%08x "
                    "size=%u)\n", g_heap_bump, size);
        pthread_mutex_unlock(&g_alloc_lock);
        return 0;
    }
    hdr_t *h = (hdr_t *)(MEM + g_heap_bump);
    h->size = size;
    h->used = 1;
    h->heap = heap;
    h->magic = HDR_MAGIC;
    uint32_t p = g_heap_bump + sizeof(hdr_t);
    g_heap_bump += sizeof(hdr_t) + size;
    memset(MEM + p, 0, size);
    if (g_verbose > 1 && p >= WATCH_LO && p < WATCH_HI)
        fprintf(stderr, "kernel32: HeapAlloc(%u) -> %08x  [manager range]\n",
                size, p);
    pthread_mutex_unlock(&g_alloc_lock);
    return p;
}

static int heap_free(uint32_t ptr)
{
    if (ptr < HEAP_ARENA_BASE || ptr >= HEAP_ARENA_END)
        return 0;
    hdr_t *h = (hdr_t *)(MEM + ptr - sizeof(hdr_t));
    if (h->magic != HDR_MAGIC)
        return 0;
    if (g_verbose > 1 && ptr >= WATCH_LO && ptr < WATCH_HI)
        fprintf(stderr, "kernel32: HeapFree(%08x) [manager range]\n", ptr);
    h->used = 0;
    return 1;
}

static uint32_t heap_size_of(uint32_t ptr)
{
    hdr_t *h = (hdr_t *)(MEM + ptr - sizeof(hdr_t));
    return h->magic == HDR_MAGIC ? h->size : 0;
}

/* --------------------------------------------------------------- kernel32 */
static void k_ExitProcess(void)
{
    fflush(stdout);
    fflush(stderr);
    exit((int)arg32(0));
}

static void k_TerminateProcess(void)
{
    exit(0);
}

static void k_GetStartupInfoA(void)
{
    uint32_t p = arg32(0);
    if (!p)
        return;
    void *p2 = MEM + p;
    memset(p2, 0, 68);
    M32(p) = 68;
    M32(p + 44) = 0;
    M32(p + 48) = 0;
    M32(p + 60) = 1;            /* STARTF_USESHOWWINDOW */
    M16(p + 64) = 1;            /* SW_SHOWNORMAL */
    RET32(0);
}

static void k_GetCommandLineA(void)
{
    uint32_t p = 0x00100000u;
    RET32(p);
}

static void k_GetEnvironmentStrings(void)
{
    uint32_t p = 0x00110000u;
    if (M8(p) == 0) {
        memcpy(MEM + p, "PATH=.\0\0", 10);
    }
    RET32(p);
}

static void k_GetEnvironmentStringsW(void) { RET32(0x00110000u); }
static void k_FreeEnvironmentStrings(void) { RET32(1); }
static void k_FreeEnvironmentStringsW(void) { RET32(1); }

static void k_GetModuleHandleA(void)
{
    RET32(pe_image_base());
}

static void k_GetModuleFileNameA(void)
{
    uint32_t buf = arg32(0), size = arg32(2);
    const char *name = "C:\\otomedius\\otomedius.exe";
    size_t n = strlen(name);
    if (size && n >= size)
        n = size - 1;
    if (buf && size) {
        memcpy(MEM + buf, name, n);
        M8(buf + n) = 0;
    }
    RET32((uint32_t)n);
}

static void k_LoadLibraryA(void)
{
    const char *name = arg_str(0);
    log_printf("kernel32: LoadLibraryA(%s)", name ? name : "(null)");
    RET32(pe_image_base());
}

static void k_FreeLibrary(void) { RET32(1); }

static int cmp_export_name(const uint8_t *image, uint32_t name_rva, const char *want)
{
    const char *n = (const char *)(image + name_rva);
    return strcmp(n, want) == 0;
}

static void k_GetProcAddress(void)
{
    uint32_t name = arg32(1);
    if (name < 0x10000) {
        RET32(0);
        return;
    }
    const char *want = (const char *)(MEM + name);
    uint32_t base = pe_image_base();
    const uint8_t *hdr = MEM + base;
    uint32_t lfanew = (uint32_t)hdr[0x3C] | ((uint32_t)hdr[0x3D] << 8) |
                      ((uint32_t)hdr[0x3E] << 16) | ((uint32_t)hdr[0x3F] << 24);
    uint32_t exp_rva = M32(base + lfanew + 24 + 96);
    if (!exp_rva) {
        RET32(0);
        return;
    }
    uint32_t n_names = M32(base + exp_rva + 24);
    uint32_t addr_funcs = M32(base + exp_rva + 28);
    uint32_t addr_names = M32(base + exp_rva + 32);
    uint32_t addr_ords = M32(base + exp_rva + 36);
    for (uint32_t i = 0; i < n_names; i++) {
        uint32_t nrva = M32(base + addr_names + i * 4);
        if (cmp_export_name(MEM + base, nrva, want)) {
            uint16_t ord = *(uint16_t *)(MEM + base + addr_ords + i * 2);
            RET32(M32(base + addr_funcs + ord * 4));
            return;
        }
    }
    RET32(0);
}

static void k_GetCurrentProcess(void) { RET32(0xFFFFFFFFu); }
static void k_GetCurrentProcessId(void) { RET32(1); }
static void k_GetCurrentThreadId(void) { RET32((uint32_t)(uintptr_t)pthread_self()); }

static void k_GetCurrentThread(void)
{
    RET32(0xFFFFFFFEu);
}

static void k_GetLastError(void) { RET32(win_last_error()); }

static void k_SetLastError(void)
{
    win_set_last_error(arg32(0));
}

static void k_GetTickCount(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    RET32((uint32_t)((uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull));
}

static void k_QueryPerformanceCounter(void)
{
    uint32_t p = arg32(0);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t v = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
    if (p) {
        M32(p) = (uint32_t)v;
        M32(p + 4) = (uint32_t)(v >> 32);
    }
    RET32(1);
}

static void k_GetSystemTimeAsFileTime(void)
{
    uint32_t p = arg32(0);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    /* 100 ns ticks since 1601-01-01 */
    uint64_t t = ((uint64_t)tv.tv_sec + 11644473600ull) * 10000000ull +
                 (uint64_t)tv.tv_usec * 10ull;
    if (p) {
        M32(p) = (uint32_t)t;
        M32(p + 4) = (uint32_t)(t >> 32);
    }
    RET32(0);
}

static void k_GetTimeZoneInformation(void)
{
    uint32_t p = arg32(0);
    if (p) {
        memset(MEM + p, 0, 68);
        M32(p) = 0;                    /* TIME_ZONE_ID_UNKNOWN */
        M32(p + 4) = 0xFFFFFFFFu;
    }
    RET32(0);
}

static void k_GetVersionExA(void)
{
    uint32_t p = arg32(0);
    if (p) {
        uint32_t size = M32(p);
        (void)size;
        M32(p + 4) = 5;      /* major */
        M32(p + 8) = 1;      /* minor */
        M32(p + 12) = 2600;  /* build */
        M32(p + 16) = 2;     /* VER_PLATFORM_WIN32_NT */
        char *csd = (char *)(MEM + p + 20);
        csd[0] = 0;
        M16(p + 276) = 0;
    }
    RET32(1);
}

static void k_GetSystemInfo(void)
{
    uint32_t p = arg32(0);
    if (p) {
        memset(MEM + p, 0, 64);
        M32(p + 0) = 0;          /* x86 */
        M32(p + 4) = 4096;       /* page size */
        M32(p + 8) = 0x10000;    /* min */
        M32(p + 12) = 0x10000000;/* max */
        M32(p + 16) = 0x7FFE0000;/* highest */
        M32(p + 20) = 1;         /* one processor */
        M32(p + 24) = 1;         /* processor mask */
        M32(p + 28) = 386;       /* architecture */
    }
    RET32(0);
}

static void k_IsDebuggerPresent(void) { RET32(0); }

static void k_SetUnhandledExceptionFilter(void) { RET32(0); }

static void k_UnhandledExceptionFilter(void)
{
    fprintf(stderr, "kernel32: unhandled exception\n");
    RET32(1);
}

static void k_RaiseException(void)
{
    fprintf(stderr, "kernel32: RaiseException(code=%08x, flags=%08x) - aborting\n",
            arg32(0), arg32(1));
    abort();
}

static void k_RtlUnwind(void)
{
    fprintf(stderr, "kernel32: RtlUnwind is not supported yet\n");
    abort();
}

static void k_Sleep(void)
{
    usleep((useconds_t)(arg32(0) * 1000u));
}

static void k_OutputDebugStringA(void)
{
    const char *s = arg_str(0);
    if (s && g_verbose)
        fprintf(stderr, "debug: %s", s);
}

static void k_SetConsoleMode(void) { RET32(1); }
static void k_GetConsoleMode(void) { RET32(1); }
static void k_GetConsoleCP(void) { RET32(932); }
static void k_GetConsoleOutputCP(void) { RET32(932); }
static void k_SetStdHandle(void) { RET32(1); }

static void k_GetStdHandle(void)
{
    uint32_t which = arg32(0);
    RET32(0xF0000000u + which);
}

static void k_SetHandleCount(void) { RET32(1); }

static void k_GetDriveTypeA(void) { RET32(3); }  /* DRIVE_FIXED */

static void k_GetFullPathNameA(void)
{
    const char *name = arg_str(0);
    uint32_t buf = arg32(1), size = arg32(2);
    if (!name || !buf || !size)
        RET32(0);
    else {
        size_t n = strlen(name);
        if (n >= size)
            n = size - 1;
        memcpy(MEM + buf, name, n);
        M8(buf + n) = 0;
        RET32((uint32_t)n);
    }
}

static void k_GetCurrentDirectoryA(void)
{
    uint32_t buf = arg32(0), size = arg32(1);
    const char *d = "C:\\otomedius";
    size_t n = strlen(d);
    if (buf && size) {
        if (n >= size)
            n = size - 1;
        memcpy(MEM + buf, d, n);
        M8(buf + n) = 0;
    }
    RET32((uint32_t)n);
}

static void k_GetLocaleInfoA(void)
{
    uint32_t lc = arg32(0), which = arg32(1), buf = arg32(3), size = arg32(4);
    const char *v = "0";
    switch (which) {
    case 0x0e: v = "1"; break;    /* LOCALE_RETURN_NUMBER off */
    case 0x10: v = "932"; break;  /* ANSI codepage */
    case 0x11: v = "932"; break;  /* OEM codepage */
    case 0x20000010: v = "932"; break;
    case 0x0c: v = "41"; break;   /* ISO country */
    case 0x1001: v = "\x93\xfa\x96\x7b\x8c\xea"; break;  /* 日本語 in SJIS */
    default: v = "1"; break;
    }
    size_t n = strlen(v);
    if (buf && size) {
        if (n >= size)
            n = size - 1;
        memcpy(MEM + buf, v, n);
        M8(buf + n) = 0;
    }
    (void)lc;
    RET32((uint32_t)(n + 1));
}

static void k_IsValidCodePage(void) { RET32(1); }
static void k_GetOEMCP(void) { RET32(932); }
static void k_GetACP(void) { RET32(932); }

static void k_GetCPInfo(void)
{
    uint32_t cp = arg32(0), p = arg32(1);
    if (p) {
        M32(p) = 2;                 /* MaxCharSize for DBCS */
        M8(p + 4) = '?';
        M16(p + 6) = 0;
    }
    (void)cp;
    RET32(1);
}

/* --------------------------------------------------- codepage conversion */
static iconv_t cp_iconv(const char *to, const char *from)
{
    return iconv_open(to, from);
}

static void k_MultiByteToWideChar(void)
{
    uint32_t cp = arg32(0), flags = arg32(1);
    const char *src = arg_str(2);
    int srclen = (int)arg32(3);
    uint32_t dst = arg32(4);
    int dstlen = (int)arg32(5);
    if (!src)
        return;
    if (srclen < 0)
        srclen = (int)strlen(src) + 1;
    if (cp == 65001) {
        /* UTF-8 to UTF-16 */
        int n = 0;
        const unsigned char *s = (const unsigned char *)src;
        int i = 0;
        while (i < srclen) {
            uint32_t cpv = s[i];
            int extra = 0;
            if (cpv >= 0xF0) { cpv &= 0x07; extra = 3; }
            else if (cpv >= 0xE0) { cpv &= 0x0F; extra = 2; }
            else if (cpv >= 0xC0) { cpv &= 0x1F; extra = 1; }
            for (int k = 0; k < extra; k++) {
                i++;
                if (i < srclen)
                    cpv = (cpv << 6) | (s[i] & 0x3F);
            }
            i++;
            if (!cpv)
                break;
            if (cpv > 0xFFFF) {
                if (dst && n + 2 <= dstlen) {
                    cpv -= 0x10000;
                    M16(dst + n * 2) = (uint16_t)(0xD800 | (cpv >> 10));
                    M16(dst + n * 2 + 2) = (uint16_t)(0xDC00 | (cpv & 0x3FF));
                }
                n += 2;
            } else {
                if (dst && n + 1 <= dstlen)
                    M16(dst + n * 2) = (uint16_t)cpv;
                n++;
            }
        }
        if (dst && n < dstlen)
            M16(dst + n * 2) = 0;
        RET32((uint32_t)n + 1);
        return;
    }
    const char *name = (cp == 932) ? "SHIFT-JIS" : (cp == 936) ? "GBK" :
                       (cp == 949) ? "EUC-KR" : (cp == 950) ? "BIG5" : "WINDOWS-1252";
    iconv_t cd = cp_iconv("UTF-16LE", name);
    if (cd == (iconv_t)-1) {
        RET32(0);
        return;
    }
    char *in = (char *)src, *out = (char *)(MEM + dst);
    size_t inleft = (size_t)srclen, outleft = (size_t)dstlen * 2;
    char tmpbuf[8192];
    if (!dst) {
        out = tmpbuf;
        outleft = sizeof(tmpbuf);
    }
    iconv(cd, &in, &inleft, &out, &outleft);
    iconv_close(cd);
    int n = (int)((dst ? (size_t)dstlen * 2 : sizeof(tmpbuf)) - outleft) / 2;
    if (dst && n < dstlen)
        M16(dst + n * 2) = 0;
    (void)flags;
    RET32((uint32_t)n + 1);
}

static void k_WideCharToMultiByte(void)
{
    uint32_t cp = arg32(0);
    const uint16_t *src = (const uint16_t *)(MEM + arg32(2));
    int srclen = (int)arg32(3);
    uint32_t dst = arg32(4);
    int dstlen = (int)arg32(5);
    if (!src)
        return;
    if (srclen < 0) {
        srclen = 0;
        while (M16(arg32(2) + srclen * 2))
            srclen++;
        srclen++;
    }
    if (cp == 65001) {
        int n = 0;
        for (int i = 0; i < srclen; i++) {
            uint32_t v = src[i];
            char out[4];
            int k = 0;
            if (!v) {
                if (dst && n < dstlen)
                    M8(dst + n) = 0;
                n++;
                break;
            }
            if (v < 0x80) { out[k++] = (char)v; }
            else if (v < 0x800) { out[k++] = (char)(0xC0 | (v >> 6)); out[k++] = (char)(0x80 | (v & 0x3F)); }
            else { out[k++] = (char)(0xE0 | (v >> 12)); out[k++] = (char)(0x80 | ((v >> 6) & 0x3F)); out[k++] = (char)(0x80 | (v & 0x3F)); }
            for (int j = 0; j < k; j++) {
                if (dst && n < dstlen)
                    M8(dst + n) = (uint8_t)out[j];
                n++;
            }
        }
        RET32((uint32_t)n);
        return;
    }
    const char *name = (cp == 932) ? "SHIFT-JIS" : (cp == 936) ? "GBK" :
                       (cp == 949) ? "EUC-KR" : (cp == 950) ? "BIG5" : "WINDOWS-1252";
    iconv_t cd = cp_iconv(name, "UTF-16LE");
    if (cd == (iconv_t)-1) {
        RET32(0);
        return;
    }
    char *in = (char *)src, *out = (char *)(MEM + dst);
    size_t inleft = (size_t)srclen * 2, outleft = (size_t)dstlen;
    char tmpbuf[8192];
    if (!dst) {
        out = tmpbuf;
        outleft = sizeof(tmpbuf);
    }
    iconv(cd, &in, &inleft, &out, &outleft);
    iconv_close(cd);
    int n = (int)((dst ? (size_t)dstlen : sizeof(tmpbuf)) - outleft);
    RET32((uint32_t)n);
}

static void k_LCMapStringA(void) { RET32(0); }
static void k_LCMapStringW(void) { RET32(0); }
static void k_GetStringTypeA(void) { RET32(1); }
static void k_GetStringTypeW(void) { RET32(1); }

static void k_CompareStringA(void)
{
    const char *a = arg_str(2), *b = arg_str(4);
    int la = (int)arg32(3), lb = (int)arg32(5);
    if (!a || !b) {
        RET32(0);
        return;
    }
    int n = strncmp(a, b, (size_t)(la < lb ? la : lb));
    if (n == 0)
        n = la - lb;
    RET32(n < 0 ? 1u : n > 0 ? 3u : 2u);
}

static void k_CompareStringW(void) { RET32(2); }

static void k_MulDiv(void)
{
    int32_t a = (int32_t)arg32(0), b = (int32_t)arg32(1), c = (int32_t)arg32(2);
    int64_t r = c ? ((int64_t)a * b) / c : -1;
    RET32((uint32_t)r);
}

/* ------------------------------------------------------------- filesystem */
#define INVALID_HANDLE 0xFFFFFFFFu

static void k_CreateFileA(void)
{
    const char *name = arg_str(0);
    uint32_t access = arg32(1), disposition = arg32(4);
    if (!name) {
        win_set_last_error(87);
        RET32(INVALID_HANDLE);
        return;
    }
    char *host = win_host_path(name);
    int flags = 0;
    switch (disposition) {
    case 1: flags = O_CREAT | O_EXCL | O_RDWR; break;     /* CREATE_NEW */
    case 2: flags = O_CREAT | O_TRUNC | O_RDWR; break;    /* CREATE_ALWAYS */
    case 3: flags = O_RDWR; break;                        /* OPEN_EXISTING */
    case 4: flags = O_CREAT | O_RDWR; break;              /* OPEN_ALWAYS */
    case 5: flags = O_CREAT | O_TRUNC | O_RDWR; break;    /* TRUNCATE_EXISTING */
    default: flags = O_RDWR; break;
    }
    if (!(access & 0x80000000u) && (access & 0x40000000u) && disposition == 4)
        flags = O_RDONLY;
    int fd = open(host, flags, 0666);
    if (fd < 0 && (disposition == 3) && (errno == EISDIR)) {
        free(host);
        win_set_last_error(5);
        RET32(INVALID_HANDLE);
        return;
    }
    if (fd < 0) {
        log_printf("kernel32: CreateFileA(%s) failed: %s", name, strerror(errno));
        free(host);
        win_set_last_error(errno == ENOENT ? 2u : 5u);
        RET32(INVALID_HANDLE);
        return;
    }
    log_printf("kernel32: CreateFileA(%s) -> fd %d", host, fd);
    free(host);
    RET32(handle_new(H_FILE, fd, NULL, 0));
}

static void k_ReadFile(void)
{
    handle_t *h = handle_get(arg32(0));
    uint32_t buf = arg32(1), n = arg32(2), readp = arg32(3);
    if (!h || h->type != H_FILE) {
        win_set_last_error(6);
        RET32(0);
        return;
    }
    ssize_t got = read(h->fd, MEM + buf, n);
    if (g_verbose > 1)
        fprintf(stderr, "kernel32: ReadFile(fd %d, %u) -> %zd\n",
                h->fd, n, got);
    if (readp) {
        M32(readp) = got > 0 ? (uint32_t)got : 0;
    }
    RET32(got >= 0 ? 1u : 0u);
}

static void k_WriteFile(void)
{
    handle_t *h = handle_get(arg32(0));
    uint32_t buf = arg32(1), n = arg32(2), writep = arg32(3);
    if (!h || h->type != H_FILE) {
        win_set_last_error(6);
        RET32(0);
        return;
    }
    ssize_t wrote = write(h->fd, MEM + buf, n);
    if (writep)
        M32(writep) = wrote > 0 ? (uint32_t)wrote : 0;
    RET32(wrote >= 0 ? 1u : 0u);
}

static void k_SetFilePointer(void)
{
    handle_t *h = handle_get(arg32(0));
    int32_t lo = (int32_t)arg32(1);
    uint32_t hip = arg32(2);
    uint32_t method = arg32(3);
    if (!h) {
        RET32(0xFFFFFFFFu);
        return;
    }
    int64_t off = (int64_t)lo;
    if (hip)
        off |= ((int64_t)M32(hip)) << 32;
    int64_t r = lseek(h->fd, off, (int)(method == 3 ? SEEK_END : method));
    RET32((uint32_t)r);
}

static void k_GetFileSize(void)
{
    handle_t *h = handle_get(arg32(0));
    uint32_t hi = arg32(1);
    if (!h) {
        RET32(0xFFFFFFFFu);
        return;
    }
    struct stat st;
    fstat(h->fd, &st);
    if (hi)
        M32(hi) = 0;
    RET32((uint32_t)st.st_size);
}

static void k_CloseHandle(void)
{
    handle_t *h = handle_get(arg32(0));
    if (!h) {
        RET32(1);
        return;
    }
    if (h->type == H_FILE && h->fd >= 0)
        close(h->fd);
    if (h->type == H_MAPPING && h->ptr)
        munmap(h->ptr, h->size);
    if (h->type == H_THREAD) {
        /* Keep thread entries alive: the game closes the handle and keeps
         * polling the same value from its loader pump, and dropping the entry
         * made WaitForSingleObject() return WAIT_FAILED forever, so the load
         * state machine never completed. */
        if (g_verbose > 1)
            fprintf(stderr, "kernel32: CloseHandle(thread %08x) kept as zombie\n",
                    arg32(0));
        RET32(1);
        return;
    }
    h->type = H_FREE;
    RET32(1);
}

static void k_GetFileType(void)
{
    handle_t *h = handle_get(arg32(0));
    RET32(h && h->type == H_FILE ? 1u : 0u);
}

static void k_GetFileAttributesA(void)
{
    const char *name = arg_str(0);
    char *host = win_host_path(name);
    struct stat st;
    if (!host || stat(host, &st) != 0) {
        free(host);
        win_set_last_error(2);
        RET32(0xFFFFFFFFu);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        free(host);
        RET32(0x10);          /* FILE_ATTRIBUTE_DIRECTORY */
        return;
    }
    char *dot = strrchr(host, '.');
    int attr = 0x80;          /* NORMAL */
    if (dot && (!strcasecmp(dot, ".dll") || !strcasecmp(dot, ".exe")))
        attr = 0x20;          /* ARCHIVE */
    free(host);
    RET32((uint32_t)attr);
}

static void k_SetFileAttributesA(void) { RET32(1); }

static void k_DeleteFileA(void)
{
    char *host = win_host_path(arg_str(0));
    int r = remove(host);
    free(host);
    RET32(r == 0 ? 1u : 0u);
}

static void k_CreateDirectoryA(void)
{
    char *host = win_host_path(arg_str(0));
    int r = mkdir(host, 0777);
    free(host);
    RET32(r == 0 || errno == EEXIST ? 1u : 0u);
}

static void k_RemoveDirectoryA(void)
{
    char *host = win_host_path(arg_str(0));
    int r = rmdir(host);
    free(host);
    RET32(r == 0 ? 1u : 0u);
}

static void k_MoveFileA(void)
{
    char *a = win_host_path(arg_str(0));
    char *b = win_host_path(arg_str(1));
    int r = rename(a, b);
    free(a);
    free(b);
    RET32(r == 0 ? 1u : 0u);
}

static void k_SetEndOfFile(void)
{
    handle_t *h = handle_get(arg32(0));
    if (h) {
        off_t cur = lseek(h->fd, 0, SEEK_CUR);
        if (ftruncate(h->fd, cur) != 0) {
            RET32(0);
            return;
        }
    }
    RET32(1);
}

static void k_FlushFileBuffers(void) { RET32(1); }

static void k_GetOverlappedResult(void) { RET32(1); }

static void k_CreateFileMappingA(void)
{
    handle_t *h = handle_get(arg32(0));
    uint32_t size = arg32(4);          /* low 32 bits of the maximum size */
    if (h && h->type == H_FILE) {
        if (!size) {
            struct stat st;
            fstat(h->fd, &st);
            size = (uint32_t)st.st_size;
        }
        RET32(handle_new(H_MAPPING, h->fd, NULL, size));
        return;
    }
    /* anonymous mapping: pages are simply zeroed on demand */
    RET32(handle_new(H_MAPPING, -1, NULL, size ? size : 0x100000));
}

static void k_MapViewOfFile(void)
{
    handle_t *h = handle_get(arg32(0));
    uint32_t size = arg32(4);
    if (!h) {
        win_set_last_error(6);
        RET32(0);
        return;
    }
    uint32_t want = size ? size : (uint32_t)h->size;
    uint32_t base = map_arena_alloc(want);
    if (!base) {
        win_set_last_error(8);          /* ERROR_NOT_ENOUGH_MEMORY */
        RET32(0);
        return;
    }
    if (h->fd >= 0 && want) {
        off_t off = (off_t)arg32(2);
        ssize_t got = pread(h->fd, MEM + base, want, off);
        if (got < 0)
            memset(MEM + base, 0, want);
        else if ((uint32_t)got < want)
            memset(MEM + base + got, 0, want - (uint32_t)got);
    }
    log_printf("kernel32: MapViewOfFile(%u bytes) -> %08x", want, base);
    RET32(base);
}

static void k_UnmapViewOfFile(void) { RET32(1); }

/* ------------------------------------------------------------- memory */
static void k_GetProcessHeap(void)
{
    static uint32_t heap;
    if (!heap)
        heap = 0x00F00000u;
    RET32(heap);
}

static void k_HeapCreate(void)
{
    static uint32_t next;
    if (!next)
        next = 0x00F00000u;
    next += 0x10000;
    RET32(next);
}

static void k_HeapDestroy(void) { RET32(1); }

static void k_HeapAlloc(void)
{
    uint32_t heap = arg32(0), flags = arg32(1), size = arg32(2);
    uint32_t p = heap_alloc(heap, size);
    if (flags & 8)          /* HEAP_ZERO_MEMORY */
        memset(MEM + p, 0, size);
    if (!p)
        fprintf(stderr, "kernel32: HeapAlloc(%u) failed (heap %08x)\n", size, heap);
    RET32(p);
}

static void k_HeapFree(void)
{
    RET32((uint32_t)heap_free(arg32(2)));
}

static void k_HeapSize(void)
{
    RET32(heap_size_of(arg32(2)));
}

static void k_HeapReAlloc(void)
{
    uint32_t heap = arg32(0), ptr = arg32(2), size = arg32(3);
    uint32_t old = heap_size_of(ptr);
    uint32_t p = heap_alloc(heap, size);
    if (p) {
        memcpy(MEM + p, MEM + ptr, old < size ? old : size);
        heap_free(ptr);
    }
    RET32(p);
}

static void k_VirtualAlloc(void)
{
    uint32_t addr = arg32(0), size = arg32(2), type = arg32(3);
    size = (size + 4095u) & ~4095u;
    if (!size)
        size = 4096;
    if (type & 0x2000) {        /* MEM_RESERVE, no commit needed */
        static uint32_t bump = PAGE_ARENA_BASE;
        if (!addr) {
            addr = bump;
            bump += size;
        }
    }
    if (!addr) {
        static uint32_t bump = PAGE_ARENA_BASE;
        addr = bump;
        bump += size;
    }
    memset(MEM + addr, 0, size);
    log_printf("kernel32: VirtualAlloc(addr=%08x size=%u type=%x) -> %08x", arg32(0),
               (unsigned)(arg32(2)), arg32(3), addr);
    RET32(addr);
}

static void k_VirtualFree(void)
{
    RET32(1);
}

static void k_VirtualProtect(void)
{
    RET32(1);
}

static void k_VirtualQuery(void) { RET32(0); }

/* ---------------------------------------------------------- interlocked */
static void k_InterlockedIncrement(void)
{
    uint32_t p = arg32(0);
    uint32_t v = M32(p) + 1;
    M32(p) = v;
    RET32(v);
}

static void k_InterlockedDecrement(void)
{
    uint32_t p = arg32(0);
    uint32_t v = M32(p) - 1;
    M32(p) = v;
    RET32(v);
}

static void k_InterlockedExchange(void)
{
    uint32_t p = arg32(0), v = arg32(1);
    uint32_t old = M32(p);
    M32(p) = v;
    RET32(old);
}

/* ------------------------------------------------------ critical sections */
#define MAX_CS 512
static pthread_mutex_t g_cs[MAX_CS];
static int g_cs_used[MAX_CS];
static pthread_mutex_t g_cs_lock = PTHREAD_MUTEX_INITIALIZER;

static int cs_index(uint32_t p, int create)
{
    pthread_mutex_lock(&g_cs_lock);
    for (int i = 0; i < MAX_CS; i++) {
        if (g_cs_used[i] && M32(p) == (uint32_t)(i + 1)) {
            pthread_mutex_unlock(&g_cs_lock);
            return i;
        }
    }
    if (!create) {
        pthread_mutex_unlock(&g_cs_lock);
        return -1;
    }
    for (int i = 0; i < MAX_CS; i++) {
        if (!g_cs_used[i]) {
            pthread_mutexattr_t attr;
            pthread_mutexattr_init(&attr);
            pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
            pthread_mutex_init(&g_cs[i], &attr);
            pthread_mutexattr_destroy(&attr);
            g_cs_used[i] = 1;
            M32(p) = (uint32_t)(i + 1);
            pthread_mutex_unlock(&g_cs_lock);
            return i;
        }
    }
    pthread_mutex_unlock(&g_cs_lock);
    return -1;
}

static void k_InitializeCriticalSection(void)
{
    cs_index(arg32(0), 1);
}

static void k_EnterCriticalSection(void)
{
    uint32_t a = arg32(0);
    int i = cs_index(a, 1);
    if (i < 0)
        return;
    if (pthread_mutex_trylock(&g_cs[i]) != 0) {
        if (g_verbose > 1)
            fprintf(stderr, "kernel32: enter cs %08x BLOCKED\n", a);
        pthread_mutex_lock(&g_cs[i]);
    }
}

static void k_LeaveCriticalSection(void)
{
    int i = cs_index(arg32(0), 0);
    if (i >= 0)
        pthread_mutex_unlock(&g_cs[i]);
}

static void k_DeleteCriticalSection(void)
{
    int i = cs_index(arg32(0), 0);
    if (i >= 0) {
        pthread_mutex_lock(&g_cs_lock);
        g_cs_used[i] = 0;
        pthread_mutex_unlock(&g_cs_lock);
    }
}

/* ------------------------------------------------------------- sync objects */
static pthread_cond_t g_never;
static int g_never_init;

static void k_CreateEventA(void)
{
    pthread_mutex_t *m = malloc(sizeof(*m));
    pthread_cond_t *c = malloc(sizeof(*c));
    pthread_mutex_init(m, NULL);
    pthread_cond_init(c, NULL);
    RET32(handle_new(H_EVENT, -1, c, 0));
    free(m);
}

static void k_SetEvent(void)
{
    handle_t *h = handle_get(arg32(0));
    if (h && h->type == H_EVENT && h->ptr)
        pthread_cond_broadcast((pthread_cond_t *)h->ptr);
    RET32(1);
}

static void k_ResetEvent(void) { RET32(1); }

static void k_CreateSemaphoreA(void)
{
    int32_t init = (int32_t)arg32(1);
    RET32(handle_new(H_SEM, -1, NULL, 0));
    handle_t *h = handle_get(HANDLE_BASE + 0);
    (void)h;
    (void)init;
}

static void k_ReleaseSemaphore(void)
{
    uint32_t prev = arg32(2);
    if (prev)
        M32(prev) = 0;
    RET32(1);
}

static void k_WaitForSingleObject(void)
{
    uint32_t h = arg32(0), ms = arg32(1);
    handle_t *hd = handle_get(h);
    if (g_verbose > 1)
        fprintf(stderr, "kernel32: WaitForSingleObject(h=%08x type=%d ms=%d)\n",
                h, hd ? hd->type : -1, (int)ms);
    if (!hd) {
        RET32(0xFFFFFFFFu);
        return;
    }
    if (hd->type == H_THREAD) {
        /* A thread wait must honour the timeout: the game polls loaders with
         * ms == 0 ("has it finished?") and joins with INFINITE.  Completion is
         * tracked by the thread itself (thread_arg_t.done) because glibc's
         * pthread_tryjoin_np() reported EBUSY for finished threads here. */
        thread_arg_t *ta = (thread_arg_t *)hd->ptr;
        if (g_verbose) {
            static uint32_t last_h;
            static int last_done = -2;
            int dn = ta ? ta->done : -1;
            if (h != last_h || dn != last_done) {
                fprintf(stderr, "kernel32:   wait thread h=%08x ta=%p done=%d\n",
                        h, (void *)ta, dn);
                last_h = h;
                last_done = dn;
            }
        }
        if (!ta || __atomic_load_n(&ta->done, __ATOMIC_ACQUIRE)) {
            RET32(0);               /* WAIT_OBJECT_0 */
            return;
        }
        if (ms == 0) {
            RET32(258);             /* WAIT_TIMEOUT */
            return;
        }
        if (ms == 0xFFFFFFFFu) {
            while (!__atomic_load_n(&ta->done, __ATOMIC_ACQUIRE))
                usleep(200);
            RET32(0);
            return;
        }
        for (uint32_t i = 0; i < ms; i++) {
            if (__atomic_load_n(&ta->done, __ATOMIC_ACQUIRE)) {
                RET32(0);
                return;
            }
            usleep(1000);
        }
        RET32(258);                 /* WAIT_TIMEOUT */
        return;
    }
    if (ms && ms != 0xFFFFFFFFu)
        usleep((useconds_t)ms * 1000u);
    RET32(0);
}

static void k_WaitForMultipleObjects(void)
{
    uint32_t count = arg32(0), arr = arg32(1), all = arg32(3), ms = arg32(4);
    (void)all;
    if (count == 0) {
        usleep((useconds_t)ms * 1000u);
        RET32(258);
        return;
    }
    if (M32(arr) == 0) {
        usleep((useconds_t)ms * 1000u);
        RET32(258);
        return;
    }
    RET32(0);
}

/* --------------------------------------------------------------- threads */

/* thread slots: a private stack and TEB each, inside the emulated space */
#define THREAD_ARENA_BASE 0x02000000u
#define THREAD_TEB_BASE   0x7FF00000u
static unsigned g_thread_slots;

static void *thread_trampoline(void *p)
{
    thread_arg_t *ta = (thread_arg_t *)p;
    x86_thread_begin(ta->start, ta->param, ta->stack_top, ta->teb);
    /* The thread says so itself: pthread_tryjoin_np() reported EBUSY even for
     * threads that had long finished, which deadlocked the game's loader. */
    __atomic_store_n(&ta->done, 1, __ATOMIC_RELEASE);
    if (g_verbose > 1)
        fprintf(stderr, "kernel32: thread done (start %08x)\n", ta->start);
    return NULL;
}

static void k_CreateThread(void)
{
    uint32_t start = arg32(2), param = arg32(3), idp = arg32(5);
    thread_arg_t *ta = malloc(sizeof(*ta));
    ta->start = start;
    ta->param = param;
    unsigned slot = g_thread_slots++;
    if (slot > 60)
        slot = 60;
    ta->stack_top = THREAD_ARENA_BASE + (slot + 1) * X86_THREAD_STACK_SIZE;
    ta->teb = THREAD_TEB_BASE + slot * 0x2000u;
    if (getenv("OTOMEDIUS_NOTHREDS")) {
        /* diagnostic switch: create the handle but never run the thread */
        ta->done = 0;
    } else {
        pthread_t t;
        int rc = pthread_create(&t, NULL, thread_trampoline, ta);
        if (rc)
            log_printf("kernel32: pthread_create failed (%d) for slot %u", rc,
                       slot);
        if (idp)
            M32(idp) = (uint32_t)(uintptr_t)t;
    }
    uint32_t hth = handle_new(H_THREAD, -1, ta, 0);
    if (g_verbose > 1) {
        fprintf(stderr, "kernel32: CreateThread(slot=%u handle=%08x fn=%08x "
                "ta=%p\n", slot, hth, param ? M32(param + 0x54) : 0,
                (void *)ta);
    }
    RET32(hth);
}

static void k_ResumeThread(void) { RET32(1); }
static void k_SetThreadPriority(void) { RET32(1); }
static void k_GetThreadPriority(void) { RET32(0); }

static void k_ExitThread(void)
{
    /* see x86_thread_exit() in main.c: never call pthread_exit() from inside
     * the dispatcher, it strands the thread inside glibc's cleanup. */
    extern int x86_thread_exit(void);
    x86_thread_exit();
}

static void k_DuplicateHandle(void)
{
    const uint8_t *a = MEM + arg32(1);
    RET32(handle_new(H_FILE, -1, NULL, 0));
    (void)a;
}

/* ------------------------------------------------------------------ TLS */
#define TLS_SLOTS 64
static pthread_key_t g_tls_key;
static int g_tls_key_init;
static int g_tls_used[TLS_SLOTS];

static void tls_key_init(void)
{
    if (!g_tls_key_init) {
        pthread_key_create(&g_tls_key, NULL);
        g_tls_key_init = 1;
    }
}

void x86_tls_set(uint32_t idx, uint32_t v);
uint32_t x86_tls_get(uint32_t idx);

static void k_TlsAlloc(void)
{
    tls_key_init();
    for (int i = 0; i < TLS_SLOTS; i++) {
        if (!g_tls_used[i]) {
            g_tls_used[i] = 1;
            RET32((uint32_t)i);
            return;
        }
    }
    RET32(0xFFFFFFFFu);
}

static void k_TlsFree(void)
{
    uint32_t i = arg32(0);
    if (i < TLS_SLOTS)
        g_tls_used[i] = 0;
    RET32(1);
}

static void k_TlsSetValue(void)
{
    x86_tls_set(arg32(0), arg32(1));
    RET32(1);
}

static void k_TlsGetValue(void)
{
    RET32(x86_tls_get(arg32(0)));
}

static void k_SetEnvironmentVariableA(void) { RET32(1); }

static void k_SetThreadPriority_impl(void) { RET32(1); }

static void k_GetEnvironmentStringsA(void) { RET32(0x00110000u); }

const import_def_t k32_imports[] = {
    {"kernel32.dll", "GetLocaleInfoA", k_GetLocaleInfoA, 4},
    {"kernel32.dll", "GetStringTypeW", k_GetStringTypeW, 4},
    {"kernel32.dll", "GetStringTypeA", k_GetStringTypeA, 5},
    {"kernel32.dll", "LCMapStringW", k_LCMapStringW, 6},
    {"kernel32.dll", "LCMapStringA", k_LCMapStringA, 6},
    {"kernel32.dll", "GetSystemTimeAsFileTime", k_GetSystemTimeAsFileTime, 1},
    {"kernel32.dll", "GetCurrentProcessId", k_GetCurrentProcessId, 0},
    {"kernel32.dll", "GetEnvironmentStringsW", k_GetEnvironmentStringsW, 0},
    {"kernel32.dll", "FreeEnvironmentStringsW", k_FreeEnvironmentStringsW, 1},
    {"kernel32.dll", "GetEnvironmentStrings", k_GetEnvironmentStrings, 0},
    {"kernel32.dll", "FreeEnvironmentStringsA", k_FreeEnvironmentStrings, 1},
    {"kernel32.dll", "RaiseException", k_RaiseException, 4},
    {"kernel32.dll", "GetModuleFileNameA", k_GetModuleFileNameA, 3},
    {"kernel32.dll", "ExitProcess", k_ExitProcess, 1},
    {"kernel32.dll", "HeapReAlloc", k_HeapReAlloc, 4},
    {"kernel32.dll", "HeapCreate", k_HeapCreate, 3},
    {"kernel32.dll", "QueryPerformanceCounter", k_QueryPerformanceCounter, 1},
    {"kernel32.dll", "HeapDestroy", k_HeapDestroy, 1},
    {"kernel32.dll", "GetDriveTypeA", k_GetDriveTypeA, 1},
    {"kernel32.dll", "GetConsoleMode", k_GetConsoleMode, 2},
    {"kernel32.dll", "GetConsoleCP", k_GetConsoleCP, 0},
    {"kernel32.dll", "SetStdHandle", k_SetStdHandle, 2},
    {"kernel32.dll", "GetStdHandle", k_GetStdHandle, 1},
    {"kernel32.dll", "SetHandleCount", k_SetHandleCount, 1},
    {"kernel32.dll", "SetFilePointer", k_SetFilePointer, 4},
    {"kernel32.dll", "SetLastError", k_SetLastError, 1},
    {"kernel32.dll", "TlsFree", k_TlsFree, 1},
    {"kernel32.dll", "TlsSetValue", k_TlsSetValue, 2},
    {"kernel32.dll", "TlsAlloc", k_TlsAlloc, 0},
    {"kernel32.dll", "TlsGetValue", k_TlsGetValue, 1},
    {"kernel32.dll", "IsValidCodePage", k_IsValidCodePage, 1},
    {"kernel32.dll", "GetOEMCP", k_GetOEMCP, 0},
    {"kernel32.dll", "GetACP", k_GetACP, 0},
    {"kernel32.dll", "GetCPInfo", k_GetCPInfo, 2},
    {"kernel32.dll", "GetTimeZoneInformation", k_GetTimeZoneInformation, 1},
    {"kernel32.dll", "WideCharToMultiByte", k_WideCharToMultiByte, 8},
    {"kernel32.dll", "RtlUnwind", k_RtlUnwind, 4},
    {"kernel32.dll", "GetStartupInfoA", k_GetStartupInfoA, 1},
    {"kernel32.dll", "GetProcessHeap", k_GetProcessHeap, 0},
    {"kernel32.dll", "GetCommandLineA", k_GetCommandLineA, 0},
    {"kernel32.dll", "ExitThread", k_ExitThread, 1},
    {"kernel32.dll", "IsDebuggerPresent", k_IsDebuggerPresent, 0},
    {"kernel32.dll", "HeapSize", k_HeapSize, 3},
    {"kernel32.dll", "UnhandledExceptionFilter", k_UnhandledExceptionFilter, 1},
    {"kernel32.dll", "TerminateProcess", k_TerminateProcess, 2},
    {"kernel32.dll", "HeapAlloc", k_HeapAlloc, 3},
    {"kernel32.dll", "HeapFree", k_HeapFree, 3},
    {"kernel32.dll", "GetFullPathNameA", k_GetFullPathNameA, 4},
    {"kernel32.dll", "SetEndOfFile", k_SetEndOfFile, 1},
    {"kernel32.dll", "CompareStringA", k_CompareStringA, 6},
    {"kernel32.dll", "CompareStringW", k_CompareStringW, 6},
    {"kernel32.dll", "FlushFileBuffers", k_FlushFileBuffers, 1},
    {"kernel32.dll", "WriteConsoleA", k_WriteFile, 5},
    {"kernel32.dll", "GetConsoleOutputCP", k_GetConsoleOutputCP, 0},
    {"kernel32.dll", "WriteConsoleW", k_WriteFile, 5},
    {"kernel32.dll", "GetCurrentDirectoryA", k_GetCurrentDirectoryA, 2},
    {"kernel32.dll", "GetFileType", k_GetFileType, 1},
    {"kernel32.dll", "CreateThread", k_CreateThread, 6},
    {"kernel32.dll", "GetVersionExA", k_GetVersionExA, 1},
    {"kernel32.dll", "GetTickCount", k_GetTickCount, 0},
    {"kernel32.dll", "GetCurrentThread", k_GetCurrentThread, 0},
    {"kernel32.dll", "GetThreadPriority", k_GetThreadPriority, 1},
    {"kernel32.dll", "InterlockedExchange", k_InterlockedExchange, 2},
    {"kernel32.dll", "GetModuleHandleA", k_GetModuleHandleA, 1},
    {"kernel32.dll", "GetProcAddress", k_GetProcAddress, 2},
    {"kernel32.dll", "LoadLibraryA", k_LoadLibraryA, 1},
    {"kernel32.dll", "FreeLibrary", k_FreeLibrary, 1},
    {"kernel32.dll", "VirtualAlloc", k_VirtualAlloc, 4},
    {"kernel32.dll", "CreateSemaphoreA", k_CreateSemaphoreA, 4},
    {"kernel32.dll", "GetCurrentProcess", k_GetCurrentProcess, 0},
    {"kernel32.dll", "DuplicateHandle", k_DuplicateHandle, 7},
    {"kernel32.dll", "VirtualFree", k_VirtualFree, 3},
    {"kernel32.dll", "GetSystemInfo", k_GetSystemInfo, 1},
    {"kernel32.dll", "ReleaseSemaphore", k_ReleaseSemaphore, 3},
    {"kernel32.dll", "InterlockedDecrement", k_InterlockedDecrement, 1},
    {"kernel32.dll", "InterlockedIncrement", k_InterlockedIncrement, 1},
    {"kernel32.dll", "WaitForMultipleObjects", k_WaitForMultipleObjects, 5},
    {"kernel32.dll", "MulDiv", k_MulDiv, 3},
    {"kernel32.dll", "ResetEvent", k_ResetEvent, 1},
    {"kernel32.dll", "SetEvent", k_SetEvent, 1},
    {"kernel32.dll", "GetCurrentThreadId", k_GetCurrentThreadId, 0},
    {"kernel32.dll", "CreateEventA", k_CreateEventA, 4},
    {"kernel32.dll", "EnterCriticalSection", k_EnterCriticalSection, 1},
    {"kernel32.dll", "UnmapViewOfFile", k_UnmapViewOfFile, 1},
    {"kernel32.dll", "DeleteCriticalSection", k_DeleteCriticalSection, 1},
    {"kernel32.dll", "InitializeCriticalSection", k_InitializeCriticalSection, 1},
    {"kernel32.dll", "ResumeThread", k_ResumeThread, 1},
    {"kernel32.dll", "SetThreadPriority", k_SetThreadPriority, 2},
    {"kernel32.dll", "LeaveCriticalSection", k_LeaveCriticalSection, 1},
    {"kernel32.dll", "MultiByteToWideChar", k_MultiByteToWideChar, 6},
    {"kernel32.dll", "CreateDirectoryA", k_CreateDirectoryA, 2},
    {"kernel32.dll", "GetOverlappedResult", k_GetOverlappedResult, 4},
    {"kernel32.dll", "WaitForSingleObject", k_WaitForSingleObject, 2},
    {"kernel32.dll", "MapViewOfFile", k_MapViewOfFile, 5},
    {"kernel32.dll", "CreateFileMappingA", k_CreateFileMappingA, 6},
    {"kernel32.dll", "ReadFile", k_ReadFile, 5},
    {"kernel32.dll", "WriteFile", k_WriteFile, 5},
    {"kernel32.dll", "SetEnvironmentVariableA", k_SetEnvironmentVariableA, 2},
    {"kernel32.dll", "GetFileSize", k_GetFileSize, 2},
    {"kernel32.dll", "CreateFileA", k_CreateFileA, 7},
    {"kernel32.dll", "GetLastError", k_GetLastError, 0},
    {"kernel32.dll", "Sleep", k_Sleep, 1},
    {"kernel32.dll", "SetUnhandledExceptionFilter", k_SetUnhandledExceptionFilter, 1},
    {"kernel32.dll", "CloseHandle", k_CloseHandle, 1},
    {"kernel32.dll", "OutputDebugStringA", k_OutputDebugStringA, 1},
    {"kernel32.dll", "SetThreadPriorityBoost", k_SetThreadPriority_impl, 2},
    {"kernel32.dll", "GetEnvironmentStringsA", k_GetEnvironmentStringsA, 0},
    {NULL, NULL, NULL, 0},
};
