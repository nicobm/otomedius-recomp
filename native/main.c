/* main.c - program entry: load the game image and start the recompiled code. */

#define _GNU_SOURCE
#include "win32.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <pthread.h>

int  x86_mem_init(void);
int  x86_load_image(const char *exe_path);
uint32_t x86_entry_address(void);
uint32_t x86_stack_top(void);
uint32_t x86_teb_address(void);
void win_set_game_dir(const char *dir);
int  x86_thread_begin(uint32_t start, uint32_t param, uint32_t stack_top, uint32_t teb);

/* A fault inside the recompiled code would only show up as "core dumped":
 * turn it into a report with the original instruction pointer. */
static void fault_handler(int sig, siginfo_t *si, void *ctx)
{
    cpu_t *c = x86_cpu();
    (void)ctx;
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
                     "\n*** otomedius-recomp: %s at recompiled address %08x\n"
                     "    eax=%08x ebx=%08x ecx=%08x edx=%08x\n"
                     "    esi=%08x edi=%08x ebp=%08x esp=%08x\n",
                     sig == SIGSEGV ? "SIGSEGV" : sig == SIGFPE ? "SIGFPE" : "signal",
                     c->eip, c->eax, c->ebx, c->ecx, c->edx,
                     c->esi, c->edi, c->ebp, c->esp);
    {
        char b2[128];
        int n2 = snprintf(b2, sizeof(b2), "    faulting address: %p  MEM=%p  "
                          "offset=%08x\n", si ? si->si_addr : NULL, (void *)MEM,
                          si ? (uint32_t)((uintptr_t)si->si_addr - (uintptr_t)MEM) : 0);
        if (n2 > 0)
            write(2, b2, (size_t)n2);
    }
    if (n > 0)
        write(2, buf, (size_t)n);
    x86_trace_dump();
    _exit(139);
}

static void dump_handler(int sig)
{
    (void)sig;
    fprintf(stderr, "[dump] %llu dispatches\n",
            (unsigned long long)x86_dispatch_count());
    x86_trace_dump();
    if (sig == SIGTERM)
        _exit(0);
}

static void install_fault_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sa.sa_handler = dump_handler;
    sigaction(SIGUSR1, &sa, NULL);
    sa.sa_handler = dump_handler;
    sigaction(SIGTERM, &sa, NULL);
}

/* The game directory: $OTOMEDIUS_DIR, the current directory, or the
 * directory of the executable. */
static int find_game_dir(char *out, size_t out_size)
{
    const char *env = getenv("OTOMEDIUS_DIR");
    char probe[4096];
    const char *candidates[3];
    candidates[0] = env;
    candidates[1] = ".";
    candidates[2] = NULL;
    for (int i = 0; candidates[i]; i++) {
        snprintf(probe, sizeof(probe), "%s/otomedius.exe", candidates[i]);
        if (access(probe, R_OK) == 0) {
            snprintf(out, out_size, "%s", candidates[i]);
            return 0;
        }
    }
    return -1;
}

static void *watchdog(void *arg)
{
    (void)arg;
    for (;;) {
        sleep(2);
        fprintf(stderr, "[watchdog] %llu calls, last targets:",
                (unsigned long long)x86_dispatch_count());
        for (int i = 0; i < 6; i++) ;
        fprintf(stderr, "\n");
    }
    return NULL;
}

static void start_watchdog(void)
{
    pthread_t t;
    pthread_create(&t, NULL, watchdog, NULL);
}

int main(int argc, char **argv)
{
    char dir[4096];
    char exe[4200];
    const char *verbose = getenv("OTOMEDIUS_LOG");
    g_verbose = verbose ? atoi(verbose) : 1;   /* logs on by default while WIP */

    if (argc > 1) {
        snprintf(dir, sizeof(dir), "%s", argv[1]);
    } else if (find_game_dir(dir, sizeof(dir)) != 0) {
        fprintf(stderr,
                "otomedius-recomp: otomedius.exe not found.\n"
                "Put your own copy of the game data in the current directory\n"
                "or point OTOMEDIUS_DIR at it.\n");
        return 1;
    }
    snprintf(exe, sizeof(exe), "%s/otomedius.exe", dir);
    install_fault_handler();
    win_set_game_dir(dir);

    if (x86_mem_init() != 0)
        return 1;
    if (x86_load_image(exe) != 0)
        return 1;
    if (platform_init() != 0)
        return 1;

    cpu_t *c = x86_cpu();
    c->esp = x86_stack_top() - 0x1000;
    c->ebp = c->esp;
    x86_push32(c, X86_RET_SENTINEL);   /* the game should never return here */
    log_printf("main: entering %08x", x86_entry_address());
    if (getenv("OTOMEDIUS_WATCHDOG"))
        start_watchdog();
    x86_dispatch(x86_entry_address());
    if (g_verbose)
        fprintf(stderr, "main: after dispatch eip=%08x esp=%08x\n", c->eip, c->esp);
    x86_run();
    if (!c->eip || c->eip == X86_RET_SENTINEL) {
        fprintf(stderr, "otomedius-recomp: the game exited %s\n",
                c->eip == X86_RET_SENTINEL ? "(it returned from WinMain)"
                                           : "prematurely (stack unwound to 0)");
        if (g_verbose > 1)
            x86_trace_dump();
    } else {
        fprintf(stderr, "otomedius-recomp: the game stopped at %08x\n", c->eip);
    }

    platform_shutdown();
    return 0;
}

/* Every game thread gets its own CPU state (thread local), its own TEB and
 * its own stack inside the emulated address space. */
int x86_thread_begin(uint32_t start, uint32_t param, uint32_t stack_top, uint32_t teb)
{
    cpu_t *c = x86_cpu();
    c->teb = teb;
    memset(MEM + teb, 0, 0x2000);
    M32(teb + 0x00) = 0xFFFFFFFFu;
    M32(teb + 0x04) = stack_top;
    M32(teb + 0x08) = stack_top - X86_THREAD_STACK_SIZE;
    M32(teb + 0x18) = teb;
    M32(teb + 0x1C) = 0x7FFDF000u;
    M32(teb + 0x28) = teb;
    M32(teb + 0x2C) = teb + 0x1000;
    c->esp = stack_top - 0x100;
    c->ebp = c->esp;
    /* a thread entry is a normal call: arguments first, return address last,
     * so the callee sees [esp] = ret and [esp+4] = param */
    x86_push32(c, param);
    x86_push32(c, X86_RET_SENTINEL);
    x86_dispatch(start);
    x86_run();
    return 0;
}

void x86_tls_set(uint32_t idx, uint32_t v)
{
    if (idx < 64)
        M32(x86_cpu()->teb + 0x1000 + idx * 4) = v;
}

uint32_t x86_tls_get(uint32_t idx)
{
    return idx < 64 ? M32(x86_cpu()->teb + 0x1000 + idx * 4) : 0;
}

void win_tick(uint32_t *lo)
{
    struct timespec ts;
    extern int clock_gettime(int, struct timespec *);
    clock_gettime(1, &ts);
    *lo = (uint32_t)((uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull);
}

/* helpers used by the Win32 layer -------------------------------- */
static uint32_t g_fake_hinstance;

uint32_t win_hinstance(void)
{
    if (!g_fake_hinstance)
        g_fake_hinstance = pe_image_base();
    return g_fake_hinstance;
}
