/* x86rt.c - runtime support for the statically recompiled Otomedius code. */

#include "x86rt.h"

extern int g_verbose;

#include <sys/mman.h>
#include <fenv.h>
#include <errno.h>
#include <time.h>

uint8_t *MEM;
const char *x86_game_dir = ".";

static __thread cpu_t *t_cpu;

void x86_register_cpu(cpu_t *c);
void x86_dump_threads(void);

cpu_t *x86_cpu(void)
{
    if (!t_cpu) {
        t_cpu = (cpu_t *)calloc(1, sizeof(cpu_t));
        if (!t_cpu) {
            fprintf(stderr, "x86: out of memory for the CPU state\n");
            abort();
        }
        t_cpu->fpu_cw = 0x027F;
        t_cpu->fpu_sw = 0;
        t_cpu->top = 0;
        t_cpu->mxcsr = 0x1F80;
        t_cpu->seg[1] = 0x23;
        t_cpu->seg[2] = 0x2B;
        t_cpu->seg[3] = 0x2B;
        x86_register_cpu(t_cpu);
    }
    return t_cpu;
}

/* ------------------------------------------------------- thread registry
 * Every emulated thread owns a cpu_t; the watchdog prints their eip so a
 * deadlock inside a worker thread is visible instead of silent. */
#define X86_MAX_CPUS 64
static cpu_t *g_cpus[X86_MAX_CPUS];
static volatile int g_ncpu;
__thread int x86_cpu_idx = -1;
const char *x86_import_name[X86_MAX_CPUS];
volatile int x86_cpu_dead[X86_MAX_CPUS];

void x86_register_cpu(cpu_t *c)
{
    int i = __atomic_fetch_add(&g_ncpu, 1, __ATOMIC_RELAXED);
    x86_cpu_idx = i;
    if (i < X86_MAX_CPUS)
        __atomic_store_n(&g_cpus[i], c, __ATOMIC_RELEASE);
}

extern __thread const char *x86_current_import;

/* OTOMEDIUS_WATCHCHG=<addr>: poll one dword once per frame and, when it
 * changes, dump the dispatch ring so the writer can be identified. */
void x86_watchchg_poll(uint32_t at)
{
    static uint32_t last;
    static int init, armed;
    const char *e = getenv("OTOMEDIUS_WATCHCHG");
    uint32_t addr, v;

    if (!e || !*e)
        return;
    /* "auto" follows the manager object's sub-state field: the slot array at
     * 0x5DE228 holds the manager pointer and the sub-state is at +8. */
    if (e[0] == 'a') {
        uint32_t mgr = M32(0x5DE228u);
        addr = mgr ? mgr + 8u : 0;
        if (!addr)
            return;
    } else {
        addr = (uint32_t)strtoul(e, NULL, 16);
    }
    if (!init) {
        last = M32(addr);
        init = 1;
        armed = 1;
        fprintf(stderr, "[chg] watching %08x = %08x\n", addr, last);
        return;
    }
    v = M32(addr);
    if (v != last) {
        fprintf(stderr, "[chg] %08x: %08x -> %08x (dispatched %08x)\n", addr,
                last, v, at);
        last = v;
        (void)armed;
    }
}

void x86_dump_threads(void)
{
    int n = __atomic_load_n(&g_ncpu, __ATOMIC_ACQUIRE);
    if (n > X86_MAX_CPUS)
        n = X86_MAX_CPUS;
    for (int i = 0; i < n; i++) {
        cpu_t *c = __atomic_load_n(&g_cpus[i], __ATOMIC_ACQUIRE);
        if (!c)
            continue;
        fprintf(stderr, "[thread %d] eip=%08x esp=%08x eax=%08x ebx=%08x "
                "ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x\n",
                i, c->eip, c->esp, c->eax, c->ebx, c->ecx, c->edx, c->esi,
                c->edi, c->ebp);
        fprintf(stderr, "[thread %d]   in native: %s%s\n", i,
                x86_import_name[i] ? x86_import_name[i] : "-",
                x86_cpu_dead[i] ? "  (thread already finished)" : "");
    }
}

/* ------------------------------------------------------------- dispatcher */
/* ring buffer of the last dispatch targets, for crash reports */
static uint32_t g_trace[64];
static unsigned g_trace_pos;
uint64_t g_dispatch_count;
uint32_t x86_ret_log[64][2];
__thread uint32_t x86_shadow[X86_SHADOW_MAX];
__thread unsigned x86_shadow_sp;
unsigned x86_ret_pos;

uint64_t x86_dispatch_count(void) { return g_dispatch_count; }

int x86_verbose_level(void) { return g_verbose; }

static const x86_entry_t *find_entry(uint32_t addr)
{
    unsigned lo = 0, hi = x86_entry_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        uint32_t a = x86_entries[mid].addr;
        if (a < addr) {
            lo = mid + 1;
        } else if (a > addr) {
            hi = mid;
        } else {
            return &x86_entries[mid];
        }
    }
    return NULL;
}

int x86_is_native_thunk(uint32_t addr);
uint32_t x86_native_entry(uint32_t thunk);

/* ---------------------------------------------------------- watch points */
/* OTOMEDIUS_WATCH="addr[:limit][,addr[:limit]]" (hex) prints every entry to
 * the given addresses with the live registers, `limit` times each (default
 * 20).  Pure diagnostic: costs nothing unless the variable is set.  Used to
 * follow the Konami boot machine and the post-boot init path without having
 * to patch the generated code. */
#define X86_WATCH_MAX 8
static struct {
    uint32_t addr;
    unsigned long limit;
    unsigned long seen;
} g_watch[X86_WATCH_MAX];
static int g_watch_len = -1;

static void x86_watch_parse(void)
{
    const char *s = getenv("OTOMEDIUS_WATCH");
    g_watch_len = 0;
    if (!s)
        return;
    while (*s && g_watch_len < X86_WATCH_MAX) {
        char *end;
        uint32_t a = (uint32_t)strtoul(s, &end, 16);
        if (end == s)
            break;
        s = end;
        unsigned long lim = 20;
        if (*s == ':') {
            lim = strtoul(s + 1, &end, 10);
            s = end;
        }
        if (*s == ',')
            s++;
        g_watch[g_watch_len].addr = a;
        g_watch[g_watch_len].limit = lim;
        g_watch[g_watch_len].seen = 0;
        g_watch_len++;
    }
}

/* OTOMEDIUS_WATCHMEM="addr[,addr...]" (hex) additionally prints one byte per
 * address on every watch hit - the quickest way to follow a game global that
 * gates a state machine. */
#define X86_PEEK_MAX 8
static uint32_t g_peek[X86_PEEK_MAX];
static int g_peek_len = -1;

static void x86_peek_parse(void)
{
    const char *s = getenv("OTOMEDIUS_WATCHMEM");
    g_peek_len = 0;
    if (!s)
        return;
    while (*s && g_peek_len < X86_PEEK_MAX) {
        char *end;
        uint32_t a = (uint32_t)strtoul(s, &end, 16);
        if (end == s)
            break;
        g_peek[g_peek_len++] = a;
        s = end;
        if (*s == ',')
            s++;
    }
}

static void x86_peek_dump(void)
{
    cpu_t *cpu;
    if (g_peek_len < 0)
        x86_peek_parse();
    cpu = x86_cpu();
    if (getenv("OTOMEDIUS_PEEKREGS")) {
        /* object layout probe: the game passes `this` in esi/ecx very often */
        fprintf(stderr,
                "[watch]   esi[%08x]=%08x +4=%08x +8=%08x +c=%08x +10=%08x "
                "| ecx[%08x]=%08x +4=%08x\n",
                cpu->esi, M32(cpu->esi), M32(cpu->esi + 4), M32(cpu->esi + 8),
                M32(cpu->esi + 0xc), M32(cpu->esi + 0x10),
                cpu->ecx, M32(cpu->ecx), M32(cpu->ecx + 4));
    }
    if (!g_peek_len)
        return;
    fprintf(stderr, "[watch]   mem:");
    for (int i = 0; i < g_peek_len; i++)
        fprintf(stderr, " %08x=%08x", g_peek[i], M32(g_peek[i]));
    fputc('\n', stderr);
}

static void x86_watch_hit(uint32_t addr)
{
    cpu_t *cpu;
    if (g_watch_len < 0)
        x86_watch_parse();
    if (!g_watch_len)
        return;
    cpu = x86_cpu();
    for (int i = 0; i < g_watch_len; i++) {
        if (g_watch[i].addr != addr)
            continue;
        if (g_watch[i].seen < g_watch[i].limit)
            fprintf(stderr,
                    "[watch] %08x #%lu esp=%08x ret=%08x eax=%08x ebx=%08x "
                    "ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x\n",
                    addr, g_watch[i].seen + 1, cpu->esp,
                    cpu->esp ? M32(cpu->esp) : 0, cpu->eax, cpu->ebx,
                    cpu->ecx, cpu->edx, cpu->esi, cpu->edi, cpu->ebp);
        if (g_watch[i].seen < g_watch[i].limit)
            x86_peek_dump();
        g_watch[i].seen++;
        if (g_watch[i].seen <= g_watch[i].limit && getenv("OTOMEDIUS_PEEKARGS")) {
            fprintf(stderr, "[watch]   stack:");
            for (int k = 1; k <= 6; k++)
                fprintf(stderr, " [esp+%d]=%08x", k * 4, M32(cpu->esp + 4 * k));
            fputc('\n', stderr);
        }
    }
}

void x86_dispatch(uint32_t addr)
{
    cpu_t *cpu = x86_cpu();
    extern void game_config_after_reset(uint32_t addr);   /* config.c */
    extern int  x86_patch_call(uint32_t addr);            /* patches.c */
    /* OTOMEDIUS_TRIPWIRE=<hex>[,<hex>...]: abort and dump when one of these
     * dwords stops looking like a code address.  Used to catch wild writes
     * into small job lists without polling every store. */
    {
        static uint32_t tw[8];
        static int tw_n = -1;
        if (tw_n < 0) {
            const char *e = getenv("OTOMEDIUS_TRIPWIRE");
            tw_n = 0;
            if (e && *e) {
                char buf[256];
                snprintf(buf, sizeof(buf), "%s", e);
                for (char *p = buf; *p && tw_n < 8; ) {
                    tw[tw_n++] = (uint32_t)strtoul(p, &p, 16);
                    if (*p == ',')
                        p++;
                }
            }
        }
        for (int i = 0; i < tw_n; i++) {
            uint32_t v = M32(tw[i]);
            if (v && (v < 0x400000u || v >= 0x5F0000u)) {
                fprintf(stderr, "tripwire: %08x = %08x after dispatch %08x "
                        "(prev %08x)\n", tw[i], v, addr, g_trace[(g_trace_pos - 1) & 63]);
                x86_trace_dump();
                abort();
            }
        }
    }
    game_config_after_reset(addr);
    if (x86_patch_call(addr))
        return;
    if (getenv("OTOMEDIUS_WATCHCHG")) {
        extern void x86_watchchg_poll(uint32_t at);
        x86_watchchg_poll(addr);
    }
    if (g_watch_len)
        x86_watch_hit(addr);
    if (g_verbose > 2)
        fprintf(stderr, "  dispatch %08x (esp=%08x top=%08x)\n", addr, cpu->esp, M32(cpu->esp));
    if (x86_is_native_thunk(addr)) {
        cpu->eip = x86_native_entry(addr);
        return;
    }
    g_trace[g_trace_pos++ & 63] = addr;
    __atomic_add_fetch(&g_dispatch_count, 1, __ATOMIC_RELAXED);
    const x86_entry_t *e = find_entry(addr);
    if (!e) {
        x86_bad_target(addr);
        return;
    }
    cpu->cont = e->cont;
    cpu->entry_esp = cpu->esp;
    cpu->esp_check = (e->cont == 0) && M8(addr) != 0xC3 && M8(addr) != 0xC2;
    e->fn();
}

uint32_t x86_call_stdcall(uint32_t target, const uint32_t *args, int nargs)
{
    cpu_t *c = x86_cpu();
    uint32_t saved_esp = c->esp, saved_eip = c->eip;
    uint32_t saved_entry = c->entry_esp;
    uint16_t saved_cont = c->cont;
    for (int i = nargs - 1; i >= 0; i--)
        x86_push32(c, args[i]);
    x86_push32(c, X86_RET_SENTINEL);
    c->eip = target;
    c->cont = 0;
    x86_run();
    uint32_t ret = c->eax;
    c->esp = saved_esp;
    c->eip = saved_eip;
    c->cont = saved_cont;
    c->entry_esp = saved_entry;
    return ret;
}

void x86_run(void)
{
    cpu_t *c = x86_cpu();
    while (c->eip && c->eip != X86_RET_SENTINEL) {
        uint32_t a = c->eip;
        c->eip = 0;
        x86_dispatch(a);
    }
}

/* ring buffer of the last dispatch targets, for crash reports */
static uint32_t g_trace[64];
static unsigned g_trace_pos;
uint64_t g_dispatch_count;
uint32_t x86_ret_log[64][2];
unsigned x86_ret_pos;



void x86_trace_dump(void)
{
    cpu_t *c = x86_cpu();
    fprintf(stderr, "last dispatch targets:\n");
    for (unsigned i = 0; i < 64; i++) {
        uint32_t a = g_trace[(g_trace_pos + i) & 63];
        if (a)
            fprintf(stderr, "  %08x", a);
        if ((i & 7) == 7)
            fputc('\n', stderr);
    }
    fprintf(stderr, "last returns (instruction -> target):\n");
    for (unsigned i = 0; i < 64; i++) {
        unsigned k = (x86_ret_pos + i) & 63;
        if (!x86_ret_log[k][0] && !x86_ret_log[k][1])
            continue;
        fprintf(stderr, "  %08x -> %08x%s\n", x86_ret_log[k][0], x86_ret_log[k][1],
                x86_ret_log[k][1] == 0 ? "   <-- returned to 0" : "");
    }
    fprintf(stderr, "regs eax={%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x\n",
            c->eax, c->ebx, c->ecx, c->edx, c->esi, c->edi, c->ebp, c->esp);
}

void x86_bad_target(uint32_t addr)
{
    cpu_t *c = x86_cpu();

    /* Diagnostic: OTOMEDIUS_BADTARGET=skip turns an unknown target into a
     * no-op that "returns" to the caller, so one run can reveal *every*
     * missing function instead of dying at the first one.  Used to find the
     * discovery gaps that only appear through computed call pointers. */
    if (getenv("OTOMEDIUS_BADTARGET")) {
        static uint32_t seen[256];
        static int nseen;
        int known = 0;
        for (int i = 0; i < nseen; i++)
            if (seen[i] == addr)
                known = 1;
        if (!known && nseen < 256) {
            seen[nseen++] = addr;
            fprintf(stderr, "[missing] %08x (caller %08x)\n", addr,
                    c->esp ? M32(c->esp) : 0);
        }
        c->eax = 0;
        c->eip = M32(c->esp);
        c->esp += 4;
        return;
    }

    x86_trace_dump();
    /* the game keeps its local heap descriptors at 0x5e1f20..0x5e1f40 */
    fprintf(stderr, "IAT: timeGetTime[54d264]=%08x DestroyWindow[54d250]=%08x "
            "Sleep[54d1c4]=%08x\n", M32(0x54d264), M32(0x54d250), M32(0x54d1c4));
    fprintf(stderr, "local heaps @5e1f26:");
    for (uint32_t a = 0x5e1f26; a < 0x5e1f3a; a += 4)
        fprintf(stderr, " %08x", M32(a));
    fprintf(stderr, "\nstack top16:");
    for (uint32_t a = c->esp - 16; a < c->esp + 48; a += 4)
        fprintf(stderr, " %08x", M32(a));
    fprintf(stderr, "\n");
    fprintf(stderr,
            "x86: no recompiled code at %08x (eip=%08x esp=%08x ebp=%08x eax=%08x)\n",
            addr, c->eip, c->esp, c->ebp, c->eax);
    fflush(stderr);
    abort();
}

void x86_bad_cont(uint32_t fn)
{
    fprintf(stderr, "x86: bad continuation in function %08x\n", fn);
    abort();
}

void x86_unimpl(cpu_t *c, uint32_t addr, const char *what)
{
    fprintf(stderr, "x86: unimplemented instruction at %08x: %s (eip=%08x)\n",
            addr, what, c->eip);
    fflush(stderr);
    abort();
}

/* ------------------------------------------------------------ native calls */
/* Provided by the Win32 layer: resolves an IAT slot to a native function. */
typedef struct {
    void (*fn)(void);
    int nargs;      /* arguments in DWORDs, popped by the callee (stdcall) */
    int stdcall;
} x86_import_t;

/* table built at start-up by imports.c */
void x86_native_call(uint32_t slot);

/* ------------------------------------------------------------------ stack */
void x86_pop32(cpu_t *c, uint32_t *dst)
{
    *dst = M32(c->esp);
    c->esp += 4;
}

void x86_pushad(cpu_t *c)
{
    uint32_t sp = c->esp;
    x86_push32(c, c->eax);
    x86_push32(c, c->ecx);
    x86_push32(c, c->edx);
    x86_push32(c, c->ebx);
    x86_push32(c, sp);
    x86_push32(c, c->ebp);
    x86_push32(c, c->esi);
    x86_push32(c, c->edi);
}

void x86_popad(cpu_t *c)
{
    c->edi = M32(c->esp); c->esp += 4;
    c->esi = M32(c->esp); c->esp += 4;
    c->ebp = M32(c->esp); c->esp += 4;
    c->esp += 4;
    c->ebx = M32(c->esp); c->esp += 4;
    c->edx = M32(c->esp); c->esp += 4;
    c->ecx = M32(c->esp); c->esp += 4;
    c->eax = M32(c->esp); c->esp += 4;
}

static uint32_t pack_flags(cpu_t *c)
{
    uint32_t f = 0x00000002u;
    if (c->cf) f |= 0x0001;
    if (c->pf) f |= 0x0004;
    if (c->af) f |= 0x0010;
    if (c->zf) f |= 0x0040;
    if (c->sf) f |= 0x0080;
    if (c->of) f |= 0x0800;
    if (c->df) f |= 0x0400;
    f |= 0x00000200u;      /* IF */
    return f;
}

static void unpack_flags(cpu_t *c, uint32_t f)
{
    c->cf = (f & 0x0001) != 0;
    c->pf = (f & 0x0004) != 0;
    c->af = (f & 0x0010) != 0;
    c->zf = (f & 0x0040) != 0;
    c->sf = (f & 0x0080) != 0;
    c->of = (f & 0x0800) != 0;
    c->df = (f & 0x0400) != 0;
}

void x86_pushfd(cpu_t *c) { x86_push32(c, pack_flags(c)); }
void x86_popfd(cpu_t *c)  { unpack_flags(c, M32(c->esp)); c->esp += 4; }

void x86_leave(cpu_t *c)
{
    c->esp = c->ebp;
    c->ebp = M32(c->esp);
    c->esp += 4;
}

void x86_enter_frame(cpu_t *c, uint32_t size, uint32_t level)
{
    x86_push32(c, c->ebp);
    uint32_t frame = c->esp;
    if (level) {
        for (uint32_t i = 1; i < level; i++) {
            c->ebp -= 4;
            x86_push32(c, M32(c->ebp));
        }
        x86_push32(c, frame);
    }
    c->ebp = frame;
    c->esp -= size;
}

/* ------------------------------------------------------------ mul/div */
void x86_mul(cpu_t *c, uint32_t src, int size)
{
    switch (size) {
    case 1: {
        uint16_t r = (uint16_t)((uint8_t)c->eax * (uint8_t)src);
        c->eax = (c->eax & 0xFFFF0000u) | r;
        c->cf = c->of = ((r & 0xFF00) != 0);
        break;
    }
    case 2: {
        uint32_t r = (uint32_t)(uint16_t)c->eax * (uint16_t)src;
        c->eax = (c->eax & 0xFFFF0000u) | (r & 0xFFFF);
        c->edx = (c->edx & 0xFFFF0000u) | (r >> 16);
        c->cf = c->of = ((r & 0xFFFF0000u) != 0);
        break;
    }
    default: {
        uint64_t r = (uint64_t)c->eax * src;
        c->eax = (uint32_t)r;
        c->edx = (uint32_t)(r >> 32);
        c->cf = c->of = (c->edx != 0);
        break;
    }
    }
}

void x86_imul1(cpu_t *c, uint32_t src, int size)
{
    switch (size) {
    case 1: {
        int16_t r = (int16_t)((int8_t)c->eax * (int8_t)src);
        c->eax = (c->eax & 0xFFFF0000u) | (uint16_t)r;
        c->cf = c->of = (r != (int8_t)r);
        break;
    }
    case 2: {
        int32_t r = (int32_t)(int16_t)c->eax * (int16_t)src;
        c->eax = (c->eax & 0xFFFF0000u) | ((uint32_t)r & 0xFFFFu);
        c->edx = (c->edx & 0xFFFF0000u) | (((uint32_t)r >> 16) & 0xFFFFu);
        c->cf = c->of = (r != (int16_t)r);
        break;
    }
    default: {
        int64_t r = (int64_t)(int32_t)c->eax * (int32_t)src;
        c->eax = (uint32_t)r;
        c->edx = (uint32_t)(r >> 32);
        c->cf = c->of = (r != (int32_t)r);
        break;
    }
    }
}

void x86_div(cpu_t *c, uint32_t src, int size)
{
    if (src == 0) {
        fprintf(stderr, "x86: division by zero at eip=%08x\n", c->eip);
        abort();
    }
    switch (size) {
    case 1: {
        uint16_t ax = (uint16_t)c->eax;
        uint16_t q = ax / (uint8_t)src, r = ax % (uint8_t)src;
        c->eax = (c->eax & 0xFFFF0000u) | (q & 0xFF) | (r << 8);
        break;
    }
    case 2: {
        uint32_t v = ((uint32_t)c->edx << 16) | (c->eax & 0xFFFF);
        uint16_t d = (uint16_t)src;
        c->eax = (c->eax & 0xFFFF0000u) | (v / d);
        c->edx = (c->edx & 0xFFFF0000u) | (v % d);
        break;
    }
    default: {
        uint64_t v = ((uint64_t)c->edx << 32) | c->eax;
        c->eax = (uint32_t)(v / src);
        c->edx = (uint32_t)(v % src);
        break;
    }
    }
}

void x86_idiv(cpu_t *c, uint32_t src, int size)
{
    if (src == 0) {
        fprintf(stderr, "x86: division by zero at eip=%08x\n", c->eip);
        abort();
    }
    switch (size) {
    case 1: {
        int16_t ax = (int16_t)c->eax;
        int8_t d = (int8_t)src;
        int8_t q = (int8_t)(ax / d), r = (int8_t)(ax % d);
        c->eax = (c->eax & 0xFFFF0000u) | (uint8_t)q | ((uint8_t)r << 8);
        break;
    }
    case 2: {
        int32_t v = (int32_t)(((uint32_t)c->edx << 16) | (c->eax & 0xFFFF));
        int16_t d = (int16_t)src;
        c->eax = (c->eax & 0xFFFF0000u) | (uint16_t)(v / d);
        c->edx = (c->edx & 0xFFFF0000u) | (uint16_t)(v % d);
        break;
    }
    default: {
        int64_t v = ((int64_t)c->edx << 32) | c->eax;
        int32_t d = (int32_t)src;
        c->eax = (uint32_t)(v / d);
        c->edx = (uint32_t)(v % d);
        break;
    }
    }
}

/* ------------------------------------------------------------- strings */
void x86_string(cpu_t *c, int kind, int size, int rep)
{
    int step = c->df ? -size : size;
    uint32_t count = (rep && c->ecx) ? c->ecx : 1;
    for (uint32_t n = 0; n < count; n++) {
        switch (kind) {
        case 0: /* movs */
            if (size == 1) M8(c->edi) = M8(c->esi);
            else if (size == 2) M16(c->edi) = M16(c->esi);
            else M32(c->edi) = M32(c->esi);
            c->esi += step;
            c->edi += step;
            break;
        case 1: /* stos */
            if (size == 1) M8(c->edi) = (uint8_t)c->eax;
            else if (size == 2) M16(c->edi) = (uint16_t)c->eax;
            else M32(c->edi) = c->eax;
            c->edi += step;
            break;
        case 2: /* lods */
            if (size == 1) c->eax = (c->eax & 0xFFFFFF00u) | M8(c->esi);
            else if (size == 2) c->eax = (c->eax & 0xFFFF0000u) | M16(c->esi);
            else c->eax = M32(c->esi);
            c->esi += step;
            break;
        case 3: { /* cmps */
            uint32_t a, b;
            if (size == 1) { a = M8(c->esi); b = M8(c->edi); }
            else if (size == 2) { a = M16(c->esi); b = M16(c->edi); }
            else { a = M32(c->esi); b = M32(c->edi); }
            x86_sub(c, a, b, size);
            c->esi += step;
            c->edi += step;
            break;
        }
        default: { /* scas */
            uint32_t a = size == 1 ? M8(c->edi) : size == 2 ? M16(c->edi) : M32(c->edi);
            uint32_t b = size == 1 ? (c->eax & 0xFF) : size == 2 ? (c->eax & 0xFFFF) : c->eax;
            x86_sub(c, a, b, size);
            c->edi += step;
            break;
        }
        }
        if (rep && c->ecx) {
            c->ecx--;
            if (rep == 1 && kind == 3 && !c->zf) break;
            if (rep == 2 && kind == 3 && c->zf) break;
            if (rep == 2 && kind == 4 && c->zf) break;
        }
    }
}

void x86_sahf(cpu_t *c)
{
    uint32_t ah = (c->eax >> 8) & 0xFF;
    c->cf = (ah & 0x01) != 0;
    c->pf = (ah & 0x04) != 0;
    c->af = (ah & 0x10) != 0;
    c->zf = (ah & 0x40) != 0;
    c->sf = (ah & 0x80) != 0;
}

void x86_lahf(cpu_t *c)
{
    uint32_t ah = 0x02;
    if (c->cf) ah |= 0x01;
    if (c->pf) ah |= 0x04;
    if (c->af) ah |= 0x10;
    if (c->zf) ah |= 0x40;
    if (c->sf) ah |= 0x80;
    c->eax = (c->eax & 0xFFFF00FFu) | (ah << 8);
}

void x86_cpuid(cpu_t *c)
{
    uint32_t leaf = c->eax, sub = c->ecx;
    uint32_t a = 0, b = 0, cc = 0, d = 0;
    if (leaf == 0) {
        a = 0x00000001;
        b = 0x756E6547; /* Genu */
        d = 0x49656E69; /* ineI */
        cc = 0x6C65746E; /* ntel */
    } else if (leaf == 1) {
        a = 0x000106A5;
        b = 0x00010800 | (1u << 16);
        cc = 0x00400103;
        d = 0x078BFBFF;
    } else if (leaf == 0x80000000) {
        a = 0x80000008;
    } else if (leaf == 0x80000001) {
        a = 0; d = 0x20100800;
    } else if (leaf == 0x80000008) {
        a = 0x00003020;
    }
    (void)sub;
    c->eax = a; c->ebx = b; c->ecx = cc; c->edx = d;
}

void x86_rdtsc(cpu_t *c)
{
    struct timespec ts;
    extern int clock_gettime(int, struct timespec *);
    clock_gettime(1 /* CLOCK_MONOTONIC */, &ts);
    uint64_t t = (uint64_t)ts.tv_sec * 3000000000ull + (uint64_t)ts.tv_nsec * 3;
    c->eax = (uint32_t)t;
    c->edx = (uint32_t)(t >> 32);
}

/* --------------------------------------------------------- fs:/gs: access */
uint32_t x86_fs_read32(cpu_t *c, uint32_t ea) { return M32(c->teb + ea); }
uint32_t x86_fs_read16(cpu_t *c, uint32_t ea) { return M16(c->teb + ea); }
uint32_t x86_fs_read8(cpu_t *c, uint32_t ea)  { return M8(c->teb + ea); }
void x86_fs_write32(cpu_t *c, uint32_t ea, uint32_t v) { M32(c->teb + ea) = v; }
void x86_fs_write16(cpu_t *c, uint32_t ea, uint32_t v) { M16(c->teb + ea) = (uint16_t)v; }
void x86_fs_write8(cpu_t *c, uint32_t ea, uint32_t v)  { M8(c->teb + ea) = (uint8_t)v; }
uint32_t x86_gs_read32(cpu_t *c, uint32_t ea) { return M32(c->gs_base + ea); }
uint32_t x86_gs_read16(cpu_t *c, uint32_t ea) { return M16(c->gs_base + ea); }
uint32_t x86_gs_read8(cpu_t *c, uint32_t ea)  { return M8(c->gs_base + ea); }
void x86_gs_write32(cpu_t *c, uint32_t ea, uint32_t v) { M32(c->gs_base + ea) = v; }
void x86_gs_write16(cpu_t *c, uint32_t ea, uint32_t v) { M16(c->gs_base + ea) = (uint16_t)v; }
void x86_gs_write8(cpu_t *c, uint32_t ea, uint32_t v)  { M8(c->gs_base + ea) = (uint8_t)v; }

/* ------------------------------------------------------------------- x87 */
#define FPU_C0 0x0100
#define FPU_C1 0x0200
#define FPU_C2 0x0400
#define FPU_C3 0x4000

long double x86_st_get(cpu_t *c, int i)
{
    return c->st[(c->top + i) & 7];
}

static void fpu_set_cc(cpu_t *c, uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3)
{
    c->fpu_cc = (uint16_t)((c0 ? FPU_C0 : 0) | (c1 ? FPU_C1 : 0) |
                           (c2 ? FPU_C2 : 0) | (c3 ? FPU_C3 : 0));
}

void x86_fld(cpu_t *c, long double v)
{
    c->top = (c->top - 1) & 7;
    c->st[c->top] = v;
    c->tag[c->top] = 1;
}

void x86_fld_mem(cpu_t *c, uint32_t ea, int bits)
{
    if (bits == 32) x86_fld(c, (long double)MF32(ea));
    else if (bits == 64) x86_fld(c, (long double)MF64(ea));
    else x86_fld(c, *(long double *)(MEM + ea));
}

void x86_fld_const(cpu_t *c, int which)
{
    switch (which) {
    case 0: x86_fld(c, 0.0L); break;
    case 1: x86_fld(c, 1.0L); break;
    case 2: x86_fld(c, 3.14159265358979323846264338327950288L); break;
    case 3: x86_fld(c, 0.301029995663981195213738894724493027L); break;
    case 4: x86_fld(c, 0.693147180559945309417232121458176568L); break;
    default: x86_fld(c, 1.44269504088896340735992468100189214L); break;
    }
}

void x86_fld_st(cpu_t *c, int i) { x86_fld(c, x86_st_get(c, i)); }

void x86_fild(cpu_t *c, uint32_t ea, int bits)
{
    switch (bits) {
    case 16: x86_fld(c, (long double)*(int16_t *)(MEM + ea)); break;
    case 32: x86_fld(c, (long double)*(int32_t *)(MEM + ea)); break;
    case 64: x86_fld(c, (long double)*(int64_t *)(MEM + ea)); break;
    default: x86_fld(c, (long double)*(int32_t *)(MEM + ea)); break;
    }
}

static long double fpu_pop(cpu_t *c)
{
    long double v = c->st[c->top];
    c->st[c->top] = 0.0L;
    c->tag[c->top] = 0;
    c->top = (c->top + 1) & 7;
    return v;
}

static long double fpu_round(long double v)
{
    long double r = nearbyintl(v);
    return r;
}

void x86_fst_mem(cpu_t *c, uint32_t ea, int bits, int pop)
{
    long double v = c->st[c->top];
    if (bits == 32) MF32(ea) = (float)v;
    else if (bits == 64) MF64(ea) = (double)v;
    else *(long double *)(MEM + ea) = v;
    if (pop) fpu_pop(c);
}

void x86_fist_mem(cpu_t *c, uint32_t ea, int bits, int pop)
{
    long double v = fpu_round(c->st[c->top]);
    long long iv = (long long)v;
    if (bits == 16) *(int16_t *)(MEM + ea) = (int16_t)iv;
    else if (bits == 32) *(int32_t *)(MEM + ea) = (int32_t)iv;
    else *(int64_t *)(MEM + ea) = iv;
    if (pop) fpu_pop(c);
}

void x86_fst_st(cpu_t *c, int i, int pop)
{
    long double v = c->st[c->top];
    if (pop) fpu_pop(c);
    c->st[(c->top + i) & 7] = v;
    c->tag[(c->top + i) & 7] = 1;
}

void x86_fxch(cpu_t *c, int i)
{
    int j = (c->top + i) & 7;
    long double v = c->st[c->top];
    c->st[c->top] = c->st[j];
    c->st[j] = v;
}

void x86_fchs(cpu_t *c) { c->st[c->top] = -c->st[c->top]; }
void x86_fabs(cpu_t *c) { c->st[c->top] = fabsl(c->st[c->top]); }
void x86_fsqrt(cpu_t *c) { c->st[c->top] = sqrtl(c->st[c->top]); }

void x86_fpu_arith(cpu_t *c, int op, int a, int b, int rev, int pop)
{
    long double x = x86_st_get(c, a);
    long double y = x86_st_get(c, b);
    long double r;
    if (rev) { long double t = x; x = y; y = t; }
    switch (op) {
    case 0: r = x + y; break;
    case 1: r = x - y; break;
    case 2: r = x * y; break;
    default: r = (y == 0.0L) ? HUGE_VALL : x / y; break;
    }
    if (pop) {
        /* Pop forms leave the result in the *destination* st(a) and then
         * discard st(0).  Writing to st(b) (= st0) and popping lost the
         * result of every `fmulp st(i)` / `faddp st(i)`. */
        c->st[(c->top + a) & 7] = r;
        c->tag[(c->top + a) & 7] = 1;
        fpu_pop(c);
    } else {
        c->st[(c->top + a) & 7] = r;
        c->tag[(c->top + a) & 7] = 1;
    }
}

void x86_fpu_arith_mem(cpu_t *c, int op, uint32_t ea, int bits, int rev, int pop)
{
    long double x = c->st[c->top];
    long double y;
    if (bits == 32) y = (long double)MF32(ea);
    else if (bits == 64) y = (long double)MF64(ea);
    else if (bits == 16) y = (long double)*(int16_t *)(MEM + ea);
    else if (bits == 80) y = *(long double *)(MEM + ea);
    else y = (long double)*(int32_t *)(MEM + ea);
    long double r;
    if (rev) { long double t = x; x = y; y = t; }
    switch (op) {
    case 0: r = x + y; break;
    case 1: r = x - y; break;
    case 2: r = x * y; break;
    default: r = (y == 0.0L) ? HUGE_VALL : x / y; break;
    }
    if (pop) fpu_pop(c);
    c->st[c->top] = r;
    c->tag[c->top] = 1;
}

void x86_fpu_cmp(cpu_t *c, int i, int pop2)
{
    long double a = c->st[c->top], b = x86_st_get(c, i);
    int u = (a != a) || (b != b);
    if (u) fpu_set_cc(c, 1, 0, 1, 1);
    else if (a > b) fpu_set_cc(c, 0, 0, 0, 0);
    else if (a < b) fpu_set_cc(c, 1, 0, 0, 0);
    else fpu_set_cc(c, 0, 0, 0, 1);
    if (pop2 >= 1) fpu_pop(c);
    if (pop2 >= 2) fpu_pop(c);
}

void x86_fpu_cmp_mem(cpu_t *c, uint32_t ea, int bits, int pop)
{
    long double a = c->st[c->top];
    long double b = bits == 32 ? (long double)MF32(ea) : (long double)MF64(ea);
    if (a > b) fpu_set_cc(c, 0, 0, 0, 0);
    else if (a < b) fpu_set_cc(c, 1, 0, 0, 0);
    else fpu_set_cc(c, 0, 0, 0, 1);
    if (pop) fpu_pop(c);
}

uint16_t x86_fpu_sw(cpu_t *c)
{
    return (uint16_t)(c->fpu_cc | ((c->top & 7) << 11) | (c->fpu_sw & 0x7F));
}

void x86_fldcw(cpu_t *c, uint16_t cw)
{
    c->fpu_cw = cw;
    switch ((cw >> 10) & 3) {
    case 0: fesetround(FE_TONEAREST); break;
    case 1: fesetround(FE_DOWNWARD); break;
    case 2: fesetround(FE_UPWARD); break;
    default: fesetround(FE_TOWARDZERO); break;
    }
}

void x86_fstenv(cpu_t *c, uint32_t ea)
{
    M32(ea) = c->fpu_cw;
    M32(ea + 4) = x86_fpu_sw(c);
    M32(ea + 8) = c->fpu_cc;
    M32(ea + 12) = 0;
    M32(ea + 16) = 0;
}

void x86_fldenv(cpu_t *c, uint32_t ea)
{
    x86_fldcw(c, (uint16_t)M32(ea));
    c->fpu_cc = (uint16_t)(M32(ea + 8) & 0x4700);
    c->fpu_sw = (uint16_t)(M32(ea + 4) & 0x7F);
}

void x86_fsave(cpu_t *c, uint32_t ea)
{
    x86_fstenv(c, ea);
    for (int i = 0; i < 8; i++) {
        long double *p = (long double *)(MEM + ea + 28 + i * 10);
        *p = c->st[(c->top + i) & 7];
    }
}

void x86_frstor(cpu_t *c, uint32_t ea)
{
    x86_fldenv(c, ea);
    for (int i = 0; i < 8; i++) {
        long double *p = (long double *)(MEM + ea + 28 + i * 10);
        c->st[(c->top + i) & 7] = *p;
        c->tag[(c->top + i) & 7] = 1;
    }
}

void x86_fsin(cpu_t *c) { c->st[c->top] = sinl(c->st[c->top]); fpu_set_cc(c, 0, 0, 1, 0); }
void x86_fcos(cpu_t *c) { c->st[c->top] = cosl(c->st[c->top]); fpu_set_cc(c, 0, 0, 1, 0); }

void x86_fptan(cpu_t *c)
{
    long double t = tanl(c->st[c->top]);
    x86_fld(c, t);
    x86_fld(c, 1.0L);
    fpu_set_cc(c, 0, 0, 1, 0);
}

void x86_fpatan(cpu_t *c)
{
    long double y = c->st[c->top];
    long double x = x86_st_get(c, 1);
    fpu_pop(c);
    c->st[c->top] = atan2l(y, x);
    fpu_set_cc(c, 0, 0, 1, 0);
}

void x86_fyl2x(cpu_t *c)
{
    long double y = c->st[c->top];
    long double x = x86_st_get(c, 1);
    fpu_pop(c);
    c->st[c->top] = y * log2l(x);
}

void x86_f2xm1(cpu_t *c) { c->st[c->top] = powl(2.0L, c->st[c->top]) - 1.0L; }
void x86_frndint(cpu_t *c) { c->st[c->top] = nearbyintl(c->st[c->top]); }

void x86_fscale(cpu_t *c)
{
    long double s = x86_st_get(c, 1);
    c->st[c->top] = ldexpl(c->st[c->top], (int)s);
}

void x86_fprem(cpu_t *c)
{
    long double a = c->st[c->top], b = x86_st_get(c, 1);
    long double q = truncl(a / b);
    long double r = a - q * b;
    c->st[c->top] = r;
    fpu_set_cc(c, 0, 0, 0, (fabsl(r) < fabsl(b) / 2) ? 1 : 0);
}

void x86_fxam(cpu_t *c)
{
    long double v = c->st[c->top];
    uint32_t c3 = 0, c2 = 0, c0 = 0;
    if (v == 0.0L) c3 = 1;
    else if (v != v) { c3 = 1; c2 = 1; c0 = 1; }
    else if (v < 0.0L) c0 = 1;
    fpu_set_cc(c, c0, 0, c2, c3);
}

void x86_fxtract(cpu_t *c)
{
    long double v = c->st[c->top];
    int e = 0;
    long double m = frexpl(v, &e);
    c->st[c->top] = (long double)e;
    x86_fld(c, m);
}

/* ------------------------------------------------------------------- SSE */
uint64_t x86_xmm_get(cpu_t *c, int r, int size)
{
    if (size == 1) return c->xmm[r][0] & 0xFFFFFFFFu;
    return c->xmm[r][0];
}

void x86_xmm_set(cpu_t *c, int r, int size, uint64_t v)
{
    if (size == 1) c->xmm[r][0] = (c->xmm[r][0] & ~0xFFFFFFFFull) | (v & 0xFFFFFFFFu);
    else if (size == 2) c->xmm[r][0] = v;
    else { c->xmm[r][0] = v; }
}

void x86_xmm_zero(cpu_t *c, int r) { c->xmm[r][0] = c->xmm[r][1] = 0; }

void x86_xmm_copy(cpu_t *c, int d, int s, int size)
{
    if (size == 1) c->xmm[d][0] = (c->xmm[d][0] & ~0xFFFFFFFFull) | (c->xmm[s][0] & 0xFFFFFFFFu);
    else if (size == 2) c->xmm[d][0] = c->xmm[s][0];
    else { c->xmm[d][0] = c->xmm[s][0]; c->xmm[d][1] = c->xmm[s][1]; }
}

void x86_sse_copy(cpu_t *c, int d, int s, int size) { x86_xmm_copy(c, d, s, size); }

uint64_t x86_sse_load(cpu_t *c, uint32_t ea, int size)
{
    (void)c;
    if (size == 1) return M32(ea);
    if (size == 2) return M64(ea);
    return M64(ea);
}

void x86_sse_store_xmm(cpu_t *c, int r, uint32_t ea, int size)
{
    if (size == 1) M32(ea) = (uint32_t)c->xmm[r][0];
    else if (size == 2) M64(ea) = c->xmm[r][0];
    else { M64(ea) = c->xmm[r][0]; M64(ea + 8) = c->xmm[r][1]; }
}

void x86_sse_load_xmm(cpu_t *c, int r, uint32_t ea, int size)
{
    if (size == 1) c->xmm[r][0] = M32(ea);
    else if (size == 2) c->xmm[r][0] = M64(ea);
    else { c->xmm[r][0] = M64(ea); c->xmm[r][1] = M64(ea + 8); }
}

uint64_t x86_xmm_get64(cpu_t *c, int r, int half) { return c->xmm[r][half & 1]; }
void x86_xmm_set64(cpu_t *c, int r, int half, uint64_t v) { c->xmm[r][half & 1] = v; }
uint32_t x86_xmm_lo32(cpu_t *c, int r) { return (uint32_t)c->xmm[r][0]; }

void x86_xmm_set_lo32(cpu_t *c, int r, uint32_t v)
{
    c->xmm[r][0] = (c->xmm[r][0] & ~0xFFFFFFFFull) | v;
}

void x86_sse_setop(cpu_t *c, int r, int size, uint64_t a, uint64_t b, int op)
{
    if (size == 1) {
        float x, y, z = 0;
        memcpy(&x, &a, 4);
        memcpy(&y, &b, 4);
        switch (op) {
        case 0: z = x + y; break;
        case 1: z = x - y; break;
        case 2: z = x * y; break;
        case 3: z = x / y; break;
        default: {
            uint32_t ua = (uint32_t)a, ub = (uint32_t)b, ur = 0;
            if (op == 4) ur = ua & ub;
            else if (op == 5) ur = (~ua) & ub;
            else if (op == 6) ur = ua | ub;
            else ur = ua ^ ub;
            c->xmm[r][0] = (c->xmm[r][0] & ~0xFFFFFFFFull) | ur;
            return;
        }
        }
        uint32_t u;
        memcpy(&u, &z, 4);
        c->xmm[r][0] = (c->xmm[r][0] & ~0xFFFFFFFFull) | u;
        return;
    }
    double x, y, z = 0;
    memcpy(&x, &a, 8);
    memcpy(&y, &b, 8);
    switch (op) {
    case 0: z = x + y; break;
    case 1: z = x - y; break;
    case 2: z = x * y; break;
    case 3: z = x / y; break;
    case 4: c->xmm[r][0] = a & b; return;
    case 5: c->xmm[r][0] = (~a) & b; return;
    case 6: c->xmm[r][0] = a | b; return;
    case 7: c->xmm[r][0] = a ^ b; return;
    }
    memcpy(&c->xmm[r][0], &z, 8);
}

void x86_sse_intop(cpu_t *c, int r, uint64_t v, int op)
{
    switch (op) {
    case 0: c->xmm[r][0] ^= v; c->xmm[r][1] ^= v; break;
    case 1: c->xmm[r][0] |= v; c->xmm[r][1] |= v; break;
    case 2: c->xmm[r][0] &= v; c->xmm[r][1] &= v; break;
    case 3: c->xmm[r][0] = (~c->xmm[r][0]) & v; c->xmm[r][1] = (~c->xmm[r][1]) & v; break;
    default: {
        int32_t vv = (int32_t)v;
        for (int i = 0; i < 4; i++) {
            int32_t d = (int32_t)(c->xmm[r][i / 2] >> ((i & 1) * 32));
            uint32_t eq = (d == vv) ? 0xFFFFFFFFu : 0;
            c->xmm[r][i / 2] = (c->xmm[r][i / 2] & ~(0xFFFFFFFFull << ((i & 1) * 32))) |
                               ((uint64_t)eq << ((i & 1) * 32));
        }
        break;
    }
    }
}

void x86_sse_comis(cpu_t *c, uint64_t a, uint64_t b)
{
    double x, y;
    memcpy(&x, &a, 8);
    memcpy(&y, &b, 8);
    c->zf = (uint8_t)((x == y) || (x != x) || (y != y));
    c->pf = (uint8_t)((x != x) || (y != y));
    c->cf = (uint8_t)(x < y);
    c->of = c->sf = c->af = 0;
}

void x86_sse_unpck(cpu_t *c, int r, uint64_t v, int high, int dwords)
{
    if (dwords == 2) {
        double d = 0, s = 0;
        memcpy(&d, &c->xmm[r][high & 1], 8);
        memcpy(&s, &v, 8);
        c->xmm[r][0] = c->xmm[r][high & 1];
        memcpy(&c->xmm[r][1], &s, 8);
        (void)d;
    } else {
        uint32_t lo = (uint32_t)(high ? c->xmm[r][1] : c->xmm[r][0]);
        c->xmm[r][1] = (c->xmm[r][1] & ~0xFFFFFFFFull) | ((uint32_t)v);
        c->xmm[r][0] = (c->xmm[r][0] & ~0xFFFFFFFFull) | lo;
    }
}

void x86_shufpd(cpu_t *c, int r, uint64_t v, int imm)
{
    uint64_t lo = (imm & 1) ? c->xmm[r][1] : c->xmm[r][0];
    uint64_t hi = (imm & 2) ? v : c->xmm[r][1];
    c->xmm[r][0] = lo;
    c->xmm[r][1] = hi;
}

void x86_pshufd(cpu_t *c, int r, int imm, uint64_t src)
{
    uint64_t full[2] = { src, 0 };
    uint64_t out[2] = { 0, 0 };
    for (int i = 0; i < 4; i++) {
        int idx = (imm >> (i * 2)) & 3;
        uint32_t v = (uint32_t)(full[idx >> 1] >> ((idx & 1) * 32));
        out[i >> 1] |= ((uint64_t)v) << ((i & 1) * 32);
    }
    c->xmm[r][0] = out[0];
    c->xmm[r][1] = out[1];
}

void x86_psrlq(cpu_t *c, int r, int cnt)
{
    if (cnt > 63) { c->xmm[r][0] = c->xmm[r][1] = 0; return; }
    c->xmm[r][0] = (c->xmm[r][0] >> cnt) | (cnt ? (c->xmm[r][1] << (64 - cnt)) : 0);
    c->xmm[r][1] = c->xmm[r][1] >> cnt;
}

void x86_psllq(cpu_t *c, int r, int cnt)
{
    if (cnt > 63) { c->xmm[r][0] = c->xmm[r][1] = 0; return; }
    c->xmm[r][1] = (c->xmm[r][1] << cnt) | (cnt ? (c->xmm[r][0] >> (64 - cnt)) : 0);
    c->xmm[r][0] = c->xmm[r][0] << cnt;
}

void x86_psrld(cpu_t *c, int r, int cnt)
{
    x86_psrlq(c, r, cnt);
}

void x86_pslld(cpu_t *c, int r, int cnt)
{
    x86_psllq(c, r, cnt);
}

void x86_psraw(cpu_t *c, int r, int cnt)
{
    for (int i = 0; i < 4; i++) {
        int16_t v = (int16_t)(c->xmm[r][i >> 1] >> ((i & 1) * 32));
        c->xmm[r][i >> 1] = (c->xmm[r][i >> 1] & ~(0xFFFFull << ((i & 1) * 32))) |
                            ((uint64_t)(uint16_t)(v >> (cnt & 15)) << ((i & 1) * 32));
    }
}

void x86_psrad(cpu_t *c, int r, int cnt)
{
    for (int i = 0; i < 2; i++) {
        int32_t v = (int32_t)(c->xmm[r][i] >> 32);
        int32_t lo = (int32_t)c->xmm[r][i];
        c->xmm[r][i] = ((uint64_t)(uint32_t)(lo >> (cnt & 31))) |
                       ((uint64_t)(uint32_t)(v >> (cnt & 31)) << 32);
    }
}

void x86_psrlw(cpu_t *c, int r, int cnt) { x86_psrlq(c, r, cnt); }
void x86_psllw(cpu_t *c, int r, int cnt) { x86_psllq(c, r, cnt); }

uint32_t x86_pmovmskb(cpu_t *c, int r)
{
    uint32_t m = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t b = (uint8_t)(c->xmm[r][i / 4] >> ((i & 3) * 8));
        if (b & 0x80) m |= 1u << i;
    }
    return m;
}

uint32_t x86_pextrw(cpu_t *c, int r, int idx)
{
    return (uint32_t)((c->xmm[r][(idx & 7) / 4] >> (((idx & 7) & 3) * 16)) & 0xFFFF);
}

void x86_pinsrw(cpu_t *c, int r, int idx, uint64_t v)
{
    int shift = ((idx & 7) & 3) * 16;
    int half = (idx & 7) / 4;
    c->xmm[r][half] = (c->xmm[r][half] & ~(0xFFFFull << shift)) | ((v & 0xFFFF) << shift);
}

void x86_pmaxsw(cpu_t *c, int r, uint64_t v)
{
    for (int i = 0; i < 4; i++) {
        int sh = (i & 1) * 32;
        int16_t a = (int16_t)(c->xmm[r][i / 2] >> sh);
        int16_t b = (int16_t)(v >> sh);
        int16_t m = a > b ? a : b;
        c->xmm[r][i / 2] = (c->xmm[r][i / 2] & ~(0xFFFFull << sh)) | ((uint64_t)(uint16_t)m << sh);
    }
}

void x86_pminsw(cpu_t *c, int r, uint64_t v)
{
    for (int i = 0; i < 4; i++) {
        int sh = (i & 1) * 32;
        int16_t a = (int16_t)(c->xmm[r][i / 2] >> sh);
        int16_t b = (int16_t)(v >> sh);
        int16_t m = a < b ? a : b;
        c->xmm[r][i / 2] = (c->xmm[r][i / 2] & ~(0xFFFFull << sh)) | ((uint64_t)(uint16_t)m << sh);
    }
}

void x86_pmulhw(cpu_t *c, int r, uint64_t v)
{
    for (int i = 0; i < 4; i++) {
        int sh = (i & 1) * 32;
        int16_t a = (int16_t)(c->xmm[r][i / 2] >> sh);
        int16_t b = (int16_t)(v >> sh);
        int16_t m = (int16_t)(((int32_t)a * b) >> 16);
        c->xmm[r][i / 2] = (c->xmm[r][i / 2] & ~(0xFFFFull << sh)) | ((uint64_t)(uint16_t)m << sh);
    }
}

void x86_pmaddwd(cpu_t *c, int r, uint64_t v)
{
    for (int i = 0; i < 2; i++) {
        int32_t res = 0;
        for (int j = 0; j < 2; j++) {
            int sh = i * 32 + j * 16;
            int16_t a = (int16_t)(c->xmm[r][0] >> sh);
            int16_t b = (int16_t)(v >> sh);
            res += (int32_t)a * b;
        }
        if (i == 0) c->xmm[r][0] = (uint32_t)res;
        else c->xmm[r][1] = (uint32_t)res;
    }
}

double x86_i2d(uint32_t v) { return (double)(int32_t)v; }

uint32_t x86_d2i(uint64_t bits)
{
    double d;
    memcpy(&d, &bits, 8);
    if (d != d) return 0x80000000u;
    if (d >= 2147483647.0) return 0x7FFFFFFFu;
    if (d <= -2147483648.0) return 0x80000000u;
    return (uint32_t)(int32_t)d;
}

double x86_sqrt(uint64_t bits)
{
    double d;
    memcpy(&d, &bits, 8);
    return sqrt(d);
}
