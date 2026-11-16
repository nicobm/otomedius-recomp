/* imports.c - binds PE imports and creates native call thunks. */

#define _GNU_SOURCE
#include "win32.h"
#include "d3d9.h"

#include <strings.h>
#include <string.h>
#include <stdlib.h>

#define THUNK_BASE  0x7F000000u
#define THUNK_STEP  16u

extern const import_def_t k32_imports[];
extern const import_def_t user32_imports[];
extern const import_def_t gdi32_imports[];
extern const import_def_t ole_imports[];
extern const import_def_t winmm_imports[];
extern const import_def_t imm32_imports[];
extern const import_def_t d3d9_imports[];
extern const import_def_t d3dx9_imports[];
extern const import_def_t dsound_imports[];
extern const import_def_t avs_imports[];

static const import_def_t *const tables[] = {
    k32_imports, user32_imports, gdi32_imports, ole_imports, winmm_imports,
    imm32_imports, d3d9_imports, d3dx9_imports, dsound_imports, avs_imports,
    NULL
};

typedef struct {
    void (*fn)(void);
    int nargs;
    int used;
    const char *name;
} nthunk_t;

static nthunk_t *g_thunks;
static uint32_t g_nthunks;

extern int g_verbose;

uint32_t x86_native_register(void (*fn)(void), int nargs)
{
    if (g_nthunks >= 0x10000u) {
        fprintf(stderr, "imports: too many native thunks\n");
        abort();
    }
    g_thunks = (nthunk_t *)realloc(g_thunks, (g_nthunks + 1) * sizeof(nthunk_t));
    g_thunks[g_nthunks].fn = fn;
    g_thunks[g_nthunks].nargs = nargs;
    g_thunks[g_nthunks].used = 1;
    g_thunks[g_nthunks].name = "";
    uint32_t addr = THUNK_BASE + g_nthunks * THUNK_STEP;
    g_nthunks++;
    return addr;
}

int x86_is_native_thunk(uint32_t addr)
{
    return addr >= THUNK_BASE && addr < THUNK_BASE + g_nthunks * THUNK_STEP;
}

uint32_t x86_native_entry(uint32_t thunk)
{
    cpu_t *c = x86_cpu();
    uint32_t idx = (thunk - THUNK_BASE) / THUNK_STEP;
    if (idx >= g_nthunks)
        x86_bad_target(thunk);
    uint32_t ret = M32(c->esp);          /* return address pushed by the call */
    (void)x86_shadow_pop(c);             /* the native wrapper pops it */
    if (g_verbose > 1 && g_thunks[idx].name[0] == 0) {
        const char *m = com_method_name(thunk);
        if (m && g_verbose > 2)
            fprintf(stderr, "  call %-32s esp=%08x ret=%08x\n", m, c->esp, ret);
    }
    if (g_verbose > 1)
        fprintf(stderr, "call %-40s esp=%08x ret=%08x (via dispatcher)\n",
                g_thunks[idx].name, c->esp, ret);
    g_thunks[idx].fn();
    c->esp += 4u * (uint32_t)g_thunks[idx].nargs + 4u;
    return ret;
}

/* `jmp [IAT]`: the native function returns to whoever called the thunk. */
void x86_native_tail(uint32_t slot)
{
    cpu_t *c = x86_cpu();
    uint32_t target = M32(slot);
    if (x86_is_native_thunk(target)) {
        uint32_t ret = M32(c->esp);
        uint32_t idx = (target - THUNK_BASE) / THUNK_STEP;
        if (g_verbose > 1)
            fprintf(stderr, "tail %-40s esp=%08x ret=%08x\n", g_thunks[idx].name,
                    c->esp, ret);
        g_thunks[idx].fn();
        c->esp += 4u * (uint32_t)g_thunks[idx].nargs + 4u;
        if (ret == 0) {
            fprintf(stderr, "x86: import tail %s returned to 0 (slot=%08x)\n",
                    g_thunks[idx].name, slot);
            x86_trace_dump();
        }
        c->eip = ret;
        return;
    }
    if (target == 0) {
        fprintf(stderr, "x86: import tail slot %08x holds 0\n", slot);
        x86_trace_dump();
    }
    c->eip = target;
}

void x86_native_invoke(uint32_t thunk)
{
    cpu_t *c = x86_cpu();
    uint32_t idx = (thunk - THUNK_BASE) / THUNK_STEP;
    if (idx >= g_nthunks)
        x86_bad_target(thunk);
    g_thunks[idx].fn();
    c->esp += 4u * (uint32_t)g_thunks[idx].nargs + 4u;  /* args + return address */
}

void x86_native_call(uint32_t slot)
{
    cpu_t *c = x86_cpu();
    uint32_t target = M32(slot);
    if (g_verbose > 2) {
        unsigned idx_ = (uint32_t)(target - THUNK_BASE) / THUNK_STEP;
        fprintf(stderr, "  native_call slot=%08x target=%08x esp=%08x nargs=%d %s\n",
                slot, target, c->esp,
                (int)g_thunks[idx_].nargs, g_thunks[idx_].name);
    }
    if (x86_is_native_thunk(target)) {
        uint32_t idx = (target - THUNK_BASE) / THUNK_STEP;
        if (g_verbose > 1)
            fprintf(stderr, "call %-40s esp=%08x (direct)\n", g_thunks[idx].name, c->esp);
        g_thunks[idx].fn();
        c->esp += 4u * (uint32_t)g_thunks[idx].nargs + 4u;
        return;
    }
    /* the game patched the import slot with its own code pointer */
    x86_dispatch(target);
    x86_run();
    c->esp += 4;
}

/* ---------------------------------------------------------- error state */
uint32_t win_last_error(void)
{
    return M32(x86_cpu()->teb + 0x34);
}

void win_set_last_error(uint32_t err)
{
    M32(x86_cpu()->teb + 0x34) = err;
}

/* ------------------------------------------------------------- binding */
static const char *g_dll, *g_fn;
static int g_missing_reported;

static void missing_import(void)
{
    static char last[128];
    char msg[128];
    snprintf(msg, sizeof(msg), "%s!%s", g_dll, g_fn);
    if (strcmp(last, msg) != 0) {
        snprintf(last, sizeof(last), "%s", msg);
        if (g_verbose)
            fprintf(stderr, "imports: %s is not implemented (returns 0)\n", msg);
    }
    win_set_last_error(127);
}

static const import_def_t *find_def(const char *dll, const char *name)
{
    for (int t = 0; tables[t]; t++) {
        for (const import_def_t *d = tables[t]; d->dll; d++) {
            if (strcasecmp(d->dll, dll) != 0)
                continue;
            if (strcmp(d->name, name) == 0)
                return d;
        }
    }
    return NULL;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

int bind_imports(uint32_t base)
{
    const uint8_t *hdr = MEM + base;
    uint32_t lfanew = rd32(hdr + 0x3C);
    const uint8_t *opt = hdr + lfanew + 24;
    uint32_t import_rva = rd32(opt + 96 + 8);
    if (!import_rva)
        return 0;
    const uint8_t *desc = MEM + base + import_rva;
    unsigned bound = 0, missing = 0;
    for (;; desc += 20) {
        uint32_t ilt = rd32(desc), name_rva = rd32(desc + 12), iat = rd32(desc + 16);
        if (!ilt && !name_rva && !iat)
            break;
        const char *dll = (const char *)(MEM + base + name_rva);
        const uint8_t *names = MEM + base + (ilt ? ilt : iat);
        for (uint32_t k = 0;; k++) {
            uint32_t v = rd32(names + k * 4);
            if (!v)
                break;
            char ordinal[32];
            const char *fname;
            if (v & 0x80000000u) {
                snprintf(ordinal, sizeof(ordinal), "ordinal:%u", v & 0xFFFF);
                fname = ordinal;
            } else {
                fname = (const char *)(MEM + base + v + 2);
            }
            g_dll = dll;
            g_fn = fname;
            const import_def_t *def = find_def(dll, fname);
            uint32_t thunk;
            if (!def) {
                thunk = x86_native_register(missing_import, 0);
                missing++;
            } else {
                thunk = x86_native_register(def->fn, def->nargs);
            }
            g_thunks[g_nthunks - 1].name = def ? def->name : fname;
            M32(base + iat + k * 4) = thunk;
            bound++;
        }
    }
    log_printf("imports: %u bound, %u missing", bound, missing);
    if (missing) {
        fprintf(stderr, "imports: %u of %u imported functions are not "
                        "implemented yet\n", missing, bound);
        g_missing_reported = 1;
    }
    (void)g_missing_reported;
    return 0;
}
