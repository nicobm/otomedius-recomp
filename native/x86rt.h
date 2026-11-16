/* x86rt.h - runtime support for the statically recompiled Otomedius code.
 *
 * The recompiled C code keeps the original 32-bit address space: `MEM` points
 * at a 4 GiB region reserved by the runtime, so every pointer the original
 * game stored in its data structures still means the same thing.
 *
 * Registers and flags live in a per-thread `cpu_t`.  A call, a return or an
 * indirect jump leaves the generated function and comes back through
 * `x86_dispatch()`, which maps the 32-bit target address to a (function,
 * continuation) pair.
 */
#ifndef X86RT_H
#define X86RT_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t *MEM;               /* the 4 GiB emulated address space */

#define X86_THREAD_STACK_SIZE (1024u * 1024u)
#define X86_RET_SENTINEL 0xFFFFFFF1u  /* "the entry point returned" */
#define X86_MEM_SIZE   ((uint64_t)0x10000u * 0x10000u)
#define M8(a)   (*(uint8_t  *)(MEM + (uint32_t)(a)))
#define M16(a)  (*(uint16_t *)(MEM + (uint32_t)(a)))
#define M32(a)  (*(uint32_t *)(MEM + (uint32_t)(a)))
#define M64(a)  (*(uint64_t *)(MEM + (uint32_t)(a)))
#define MF32(a) (*(float    *)(MEM + (uint32_t)(a)))
#define MF64(a) (*(double   *)(MEM + (uint32_t)(a)))

typedef struct {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t eip;
    uint8_t  cf, pf, af, zf, sf, of, df;
    uint16_t cont;                  /* continuation inside the current function */
    uint16_t seg[6];                /* es, cs, ss, ds, fs, gs (selectors only) */

    uint64_t xmm[8][2];
    uint32_t mxcsr;

    long double st[8];              /* x87 register file, physical order */
    uint8_t  tag[8];                /* 0 empty, 1 valid, 2 special */
    int8_t   top;                   /* physical index of ST(0) */
    uint16_t fpu_cw, fpu_sw, fpu_cc;

    uint32_t entry_esp;             /* esp when the current function was entered */
    uint8_t  esp_check;             /* 1 when entered at a real function entry */
    uint32_t teb;                   /* linear address of the fake TEB (fs:) */
    uint32_t gs_base;
    void    *host;                  /* host thread handle */
} cpu_t;

typedef struct {
    uint32_t addr;
    uint16_t cont;
    void   (*fn)(void);
} x86_entry_t;

extern const x86_entry_t x86_entries[];
extern const unsigned x86_entry_count;

cpu_t *x86_cpu(void);
void   x86_dispatch(uint32_t addr);
void   x86_run(void);
void   x86_native_call(uint32_t slot);
void   x86_native_tail(uint32_t slot);
void   x86_native_invoke(uint32_t thunk);
int    x86_is_native_thunk(uint32_t addr);
uint32_t x86_native_entry(uint32_t thunk);
uint32_t x86_native_register(void (*fn)(void), int nargs);
void   x86_unimpl(cpu_t *c, uint32_t addr, const char *what);
void   x86_bad_cont(uint32_t fn);
void   x86_bad_target(uint32_t addr);
void   x86_trace_dump(void);
uint64_t x86_dispatch_count(void);

/* ---------------------------------------------------------------- registers */
static inline uint32_t x86_r8(cpu_t *c, int i)
{
    switch (i) {
    case 0: return c->eax & 0xFF;
    case 1: return c->ecx & 0xFF;
    case 2: return c->edx & 0xFF;
    case 3: return c->ebx & 0xFF;
    case 4: return (c->eax >> 8) & 0xFF;
    case 5: return (c->ecx >> 8) & 0xFF;
    case 6: return (c->edx >> 8) & 0xFF;
    default: return (c->ebx >> 8) & 0xFF;
    }
}

static inline void x86_r8_set(cpu_t *c, int i, uint32_t v)
{
    switch (i) {
    case 0: c->eax = (c->eax & ~0xFFu) | (v & 0xFF); break;
    case 1: c->ecx = (c->ecx & ~0xFFu) | (v & 0xFF); break;
    case 2: c->edx = (c->edx & ~0xFFu) | (v & 0xFF); break;
    case 3: c->ebx = (c->ebx & ~0xFFu) | (v & 0xFF); break;
    case 4: c->eax = (c->eax & ~0xFF00u) | ((v & 0xFF) << 8); break;
    case 5: c->ecx = (c->ecx & ~0xFF00u) | ((v & 0xFF) << 8); break;
    case 6: c->edx = (c->edx & ~0xFF00u) | ((v & 0xFF) << 8); break;
    default: c->ebx = (c->ebx & ~0xFF00u) | ((v & 0xFF) << 8); break;
    }
}

static inline uint32_t x86_signext(uint32_t v, int bits)
{
    switch (bits) {
    case 8:  return (uint32_t)(int32_t)(int8_t)(v & 0xFF);
    case 16: return (uint32_t)(int32_t)(int16_t)(v & 0xFFFF);
    default: return v;
    }
}

/* ------------------------------------------------------------------- flags */
#define SIGN(v, size) (((v) >> ((size) * 8 - 1)) & 1)

static inline void x86_parity(cpu_t *c, uint32_t v)
{
    v &= 0xFF;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    c->pf = (uint8_t)((~v) & 1);
}

static inline void x86_szp(cpu_t *c, uint32_t v, int size)
{
    uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1);
    v &= mask;
    c->zf = (uint8_t)(v == 0);
    c->sf = (uint8_t)SIGN(v, size);
    x86_parity(c, v);
}

static inline uint32_t x86_mask(uint32_t v, int size)
{
    return size == 4 ? v : (v & ((1u << (size * 8)) - 1));
}

/* --------------------------------------------------------------- arithmetic */
static inline uint32_t x86_add(cpu_t *c, uint32_t a, uint32_t b, int size)
{
    uint64_t ua = x86_mask(a, size), ub = x86_mask(b, size);
    uint32_t r = x86_mask((uint32_t)(ua + ub), size);
    int bits = size * 8;
    c->cf = (uint8_t)((ua + ub) >> bits) & 1;
    c->of = (uint8_t)(((ua ^ r) & (ub ^ r)) >> (bits - 1) & 1);
    c->af = (uint8_t)(((ua ^ ub ^ r) >> 4) & 1);
    x86_szp(c, r, size);
    return r;
}

static inline uint32_t x86_adc(cpu_t *c, uint32_t a, uint32_t b, int size)
{
    uint64_t ua = x86_mask(a, size), ub = x86_mask(b, size), k = c->cf;
    uint32_t r = x86_mask((uint32_t)(ua + ub + k), size);
    int bits = size * 8;
    c->cf = (uint8_t)((ua + ub + k) >> bits) & 1;
    c->of = (uint8_t)(((ua ^ r) & (ub ^ r)) >> (bits - 1) & 1);
    c->af = (uint8_t)(((ua ^ ub ^ r) >> 4) & 1);
    x86_szp(c, r, size);
    return r;
}

static inline uint32_t x86_sub(cpu_t *c, uint32_t a, uint32_t b, int size)
{
    uint32_t ua = x86_mask(a, size), ub = x86_mask(b, size);
    uint32_t r = x86_mask(ua - ub, size);
    int bits = size * 8;
    c->cf = (uint8_t)(ua < ub);
    c->of = (uint8_t)(((ua ^ ub) & (ua ^ r)) >> (bits - 1) & 1);
    c->af = (uint8_t)(((ua ^ ub ^ r) >> 4) & 1);
    x86_szp(c, r, size);
    return r;
}

static inline uint32_t x86_sbb(cpu_t *c, uint32_t a, uint32_t b, int size)
{
    uint32_t ua = x86_mask(a, size), ub = x86_mask(b, size), k = c->cf;
    uint32_t r = x86_mask(ua - ub - k, size);
    int bits = size * 8;
    c->cf = (uint8_t)((uint64_t)ua < (uint64_t)ub + k);
    c->of = (uint8_t)(((ua ^ ub) & (ua ^ r)) >> (bits - 1) & 1);
    c->af = (uint8_t)(((ua ^ ub ^ r) >> 4) & 1);
    x86_szp(c, r, size);
    return r;
}

static inline uint32_t x86_and(cpu_t *c, uint32_t a, uint32_t b, int size)
{
    uint32_t r = x86_mask(a & b, size);
    c->cf = 0; c->of = 0;
    x86_szp(c, r, size);
    return r;
}

static inline uint32_t x86_or(cpu_t *c, uint32_t a, uint32_t b, int size)
{
    uint32_t r = x86_mask(a | b, size);
    c->cf = 0; c->of = 0;
    x86_szp(c, r, size);
    return r;
}

static inline uint32_t x86_xor(cpu_t *c, uint32_t a, uint32_t b, int size)
{
    uint32_t r = x86_mask(a ^ b, size);
    c->cf = 0; c->of = 0;
    x86_szp(c, r, size);
    return r;
}

static inline uint32_t x86_cmp(cpu_t *c, uint32_t a, uint32_t b, int size)
{
    x86_sub(c, a, b, size);
    return x86_mask(a, size);
}

static inline void x86_test(cpu_t *c, uint32_t a, uint32_t b, int size)
{
    x86_and(c, a, b, size);
}

static inline uint32_t x86_inc(cpu_t *c, uint32_t a, int size)
{
    uint8_t cf = c->cf;
    uint32_t r = x86_add(c, a, 1, size);
    c->cf = cf;
    return r;
}

static inline uint32_t x86_dec(cpu_t *c, uint32_t a, int size)
{
    uint8_t cf = c->cf;
    uint32_t r = x86_sub(c, a, 1, size);
    c->cf = cf;
    return r;
}

static inline uint32_t x86_neg(cpu_t *c, uint32_t a, int size)
{
    return x86_sub(c, 0, a, size);
}

/* --------------------------------------------------------------- shifts */
static inline uint32_t x86_shl(cpu_t *c, uint32_t a, uint32_t cnt, int size)
{
    int bits = size * 8;
    uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << bits) - 1);
    uint32_t r;
    cnt &= 31;
    if (cnt == 0) return a & mask;
    a &= mask;
    r = (uint32_t)((uint64_t)a << cnt) & mask;
    c->cf = (uint8_t)((a >> (bits - cnt)) & 1);
    c->of = (uint8_t)(((r >> (bits - 1)) & 1) ^ c->cf);
    x86_szp(c, r, size);
    return r;
}

static inline uint32_t x86_shr(cpu_t *c, uint32_t a, uint32_t cnt, int size)
{
    int bits = size * 8;
    uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << bits) - 1);
    uint32_t r;
    cnt &= 31;
    if (cnt == 0) return a & mask;
    a &= mask;
    r = cnt >= (uint32_t)bits ? 0 : (a >> cnt);
    c->cf = cnt > (uint32_t)bits ? 0 : (uint8_t)((a >> (cnt - 1)) & 1);
    c->of = (uint8_t)((a >> (bits - 1)) & 1);
    x86_szp(c, r, size);
    return r;
}

static inline uint32_t x86_sar(cpu_t *c, uint32_t a, uint32_t cnt, int size)
{
    int bits = size * 8;
    uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << bits) - 1);
    int32_t s = (int32_t)x86_signext(a & mask, bits);
    uint32_t r;
    cnt &= 31;
    if (cnt == 0) return a & mask;
    if (cnt >= (uint32_t)bits) cnt = bits;
    r = x86_mask((uint32_t)(s >> cnt), size);
    c->cf = (uint8_t)((x86_mask(a, size) >> (cnt - 1)) & 1);
    c->of = 0;
    x86_szp(c, r, size);
    return r;
}

static inline uint32_t x86_rol(cpu_t *c, uint32_t a, uint32_t cnt, int size)
{
    int bits = size * 8;
    uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << bits) - 1);
    a &= mask;
    cnt &= 31;
    if (cnt == 0) return a;
    cnt %= bits;
    if (cnt) a = ((a << cnt) | (a >> (bits - cnt))) & mask;
    c->cf = (uint8_t)(a & 1);
    c->of = (uint8_t)(((a >> (bits - 1)) & 1) ^ c->cf);
    return a;
}

static inline uint32_t x86_ror(cpu_t *c, uint32_t a, uint32_t cnt, int size)
{
    int bits = size * 8;
    uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << bits) - 1);
    a &= mask;
    cnt &= 31;
    if (cnt == 0) return a;
    cnt %= bits;
    if (cnt) a = ((a >> cnt) | (a << (bits - cnt))) & mask;
    c->cf = (uint8_t)((a >> (bits - 1)) & 1);
    c->of = (uint8_t)(((a >> (bits - 1)) & 1) ^ ((a >> (bits - 2)) & 1));
    return a;
}

static inline uint32_t x86_rcl(cpu_t *c, uint32_t a, uint32_t cnt, int size)
{
    int bits = size * 8;
    uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << bits) - 1);
    uint32_t r = a & mask, k = c->cf & 1;
    cnt &= 31;
    if (cnt == 0) return r;
    cnt %= (bits + 1);
    while (cnt--) {
        uint32_t hi = (r >> (bits - 1)) & 1;
        r = ((r << 1) | k) & mask;
        k = hi;
    }
    c->cf = (uint8_t)k;
    c->of = (uint8_t)(((r >> (bits - 1)) & 1) ^ c->cf);
    return r;
}

static inline uint32_t x86_rcr(cpu_t *c, uint32_t a, uint32_t cnt, int size)
{
    int bits = size * 8;
    uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << bits) - 1);
    uint32_t r = a & mask, k = c->cf & 1;
    cnt &= 31;
    if (cnt == 0) return r;
    cnt %= (bits + 1);
    while (cnt--) {
        uint32_t lo = r & 1;
        r = (r >> 1) | (k << (bits - 1));
        k = lo;
    }
    c->cf = (uint8_t)k;
    c->of = (uint8_t)(((r >> (bits - 1)) & 1) ^ ((r >> (bits - 2)) & 1));
    return r;
}

static inline uint32_t x86_shld(cpu_t *c, uint32_t d, uint32_t s, uint32_t cnt, int size)
{
    int bits = size * 8;
    uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << bits) - 1);
    d &= mask; s &= mask;
    cnt &= 31;
    if (cnt == 0) return d;
    if (cnt > (uint32_t)bits) return d;
    {
        uint64_t v = ((uint64_t)d << 32) | s;
        uint32_t r = (uint32_t)((v >> (32 - cnt)) & mask);
        c->cf = (uint8_t)((d >> (bits - cnt)) & 1);
        x86_szp(c, r, size);
        return r;
    }
}

static inline uint32_t x86_shrd(cpu_t *c, uint32_t d, uint32_t s, uint32_t cnt, int size)
{
    int bits = size * 8;
    uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << bits) - 1);
    d &= mask; s &= mask;
    cnt &= 31;
    if (cnt == 0) return d;
    if (cnt > (uint32_t)bits) return d;
    {
        uint64_t v = ((uint64_t)s << 32) | d;
        uint32_t r = (uint32_t)((v >> cnt) & mask);
        c->cf = (uint8_t)((d >> (cnt - 1)) & 1);
        x86_szp(c, r, size);
        return r;
    }
}

static inline uint32_t x86_bt(cpu_t *c, uint32_t a, uint32_t bit, int size)
{
    int bits = size * 8;
    uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << bits) - 1);
    c->cf = (uint8_t)(((a & mask) >> (bit & (bits - 1))) & 1);
    return a;
}

static inline uint32_t x86_bts(cpu_t *c, uint32_t a, uint32_t bit, int size)
{
    uint32_t r = x86_bt(c, a, bit, size);
    return r | (1u << (bit & (size * 8 - 1)));
}

static inline uint32_t x86_btr(cpu_t *c, uint32_t a, uint32_t bit, int size)
{
    uint32_t r = x86_bt(c, a, bit, size);
    return r & ~(1u << (bit & (size * 8 - 1)));
}

static inline uint32_t x86_btc(cpu_t *c, uint32_t a, uint32_t bit, int size)
{
    uint32_t r = x86_bt(c, a, bit, size);
    return r ^ (1u << (bit & (size * 8 - 1)));
}

/* ------------------------------------------------------------ mul/div */
void x86_mul(cpu_t *c, uint32_t src, int size);
void x86_div(cpu_t *c, uint32_t src, int size);
void x86_idiv(cpu_t *c, uint32_t src, int size);
void x86_imul1(cpu_t *c, uint32_t src, int size);
static inline uint32_t x86_imul2(cpu_t *c, uint32_t a, uint32_t b, int size)
{
    uint32_t r;
    switch (size) {
    case 1: r = (uint8_t)((int8_t)a * (int8_t)b); break;
    case 2: r = (uint16_t)((int16_t)a * (int16_t)b); break;
    default: r = (uint32_t)((int32_t)a * (int32_t)b); break;
    }
    c->of = c->cf = (uint8_t)(r != x86_signext(r, size * 8) || 0);
    return r;
}

/* --------------------------------------------------------------- stack */
/* every dynamic transfer goes through this: a zero target is always a bug */
static inline void x86_jump(cpu_t *c, uint32_t target)
{
    if (__builtin_expect(target == 0, 0))
        x86_bad_target(0);
    c->eip = target;
}

static inline void x86_push32(cpu_t *c, uint32_t v)
{
    c->esp -= 4;
    M32(c->esp) = v;
}
static inline uint32_t x86_peek32(cpu_t *c) { return M32(c->esp); }
/* Shadow return stack: every emulated call pushes its return address here and
 * every ret checks it, so a stack imbalance is reported at the exact site. */
#define X86_SHADOW_MAX 4096
extern __thread uint32_t x86_shadow[X86_SHADOW_MAX];
extern __thread unsigned x86_shadow_sp;
int x86_verbose_level(void);

static inline void x86_shadow_push(cpu_t *c, uint32_t ret) { (void)c; (void)ret; }
static inline uint32_t x86_shadow_pop(cpu_t *c) { (void)c; return 0; }

/* ring buffer of the last returns, for crash reports */
extern uint32_t x86_ret_log[64][2];
extern unsigned x86_ret_pos;

static inline uint32_t x86_ret_from(cpu_t *c, uint32_t at)
{
    uint32_t a = M32(c->esp);
    c->esp += 4;
    /* a function must leave the stack exactly where it found it, plus the
     * bytes its own `ret N` pops */
    {
        uint32_t want = c->entry_esp + 4;
        uint8_t op = M8(at);
        if (op == 0xC2)                 /* ret imm16 */
            want += M16(at + 1);
        if (c->esp_check && __builtin_expect(c->esp != want, 0)) {
            if (x86_verbose_level() > 1)
                fprintf(stderr, "x86: IMBALANCE ret at %08x: esp=%08x, entered with %08x "
                        "(expected %08x)\n", at, c->esp, c->entry_esp, want);
        }
    }
    if (0) {
        if (x86_verbose_level() > 1)
            fprintf(stderr, "x86: ret at %08x leaves esp=%08x, entered with %08x "
                    "(expected %08x)\n", at, c->esp, c->entry_esp, c->entry_esp + 4);
    }
    x86_ret_log[x86_ret_pos & 63][0] = at;
    x86_ret_log[x86_ret_pos & 63][1] = a;
    x86_ret_pos++;
    return a;
}

static inline uint32_t x86_retaddr(cpu_t *c)
{
    uint32_t a = M32(c->esp);
    c->esp += 4;
    return a;
}
void x86_pop32(cpu_t *c, uint32_t *dst);
void x86_pushad(cpu_t *c);
void x86_popad(cpu_t *c);
void x86_pushfd(cpu_t *c);
void x86_popfd(cpu_t *c);
void x86_leave(cpu_t *c);
void x86_enter_frame(cpu_t *c, uint32_t size, uint32_t level);

/* --------------------------------------------------------------- strings */
void x86_string(cpu_t *c, int kind, int size, int rep);
void x86_sahf(cpu_t *c);
void x86_lahf(cpu_t *c);
void x86_cpuid(cpu_t *c);
void x86_rdtsc(cpu_t *c);

/* segment overrides: fs:/gs: go through the fake TEB */
uint32_t x86_fs_read32(cpu_t *c, uint32_t ea);
uint32_t x86_fs_read16(cpu_t *c, uint32_t ea);
uint32_t x86_fs_read8(cpu_t *c, uint32_t ea);
void x86_fs_write32(cpu_t *c, uint32_t ea, uint32_t v);
void x86_fs_write16(cpu_t *c, uint32_t ea, uint32_t v);
void x86_fs_write8(cpu_t *c, uint32_t ea, uint32_t v);
uint32_t x86_gs_read32(cpu_t *c, uint32_t ea);
uint32_t x86_gs_read16(cpu_t *c, uint32_t ea);
uint32_t x86_gs_read8(cpu_t *c, uint32_t ea);
void x86_gs_write32(cpu_t *c, uint32_t ea, uint32_t v);
void x86_gs_write16(cpu_t *c, uint32_t ea, uint32_t v);
void x86_gs_write8(cpu_t *c, uint32_t ea, uint32_t v);

/* ---------------------------------------------------------------- x87 */
long double x86_st_get(cpu_t *c, int i);
void   x86_fld(cpu_t *c, long double v);
void   x86_fld_mem(cpu_t *c, uint32_t ea, int bits);
void   x86_fld_const(cpu_t *c, int which);
void   x86_fld_st(cpu_t *c, int i);
void   x86_fild(cpu_t *c, uint32_t ea, int bits);
void   x86_fst_mem(cpu_t *c, uint32_t ea, int bits, int pop);
void   x86_fist_mem(cpu_t *c, uint32_t ea, int bits, int pop);
void   x86_fst_st(cpu_t *c, int i, int pop);
void   x86_fxch(cpu_t *c, int i);
void   x86_fchs(cpu_t *c);
void   x86_fabs(cpu_t *c);
void   x86_fsqrt(cpu_t *c);
void   x86_fpu_arith(cpu_t *c, int op, int a, int b, int rev, int pop);
void   x86_fpu_arith_mem(cpu_t *c, int op, uint32_t ea, int bits, int rev, int pop);
void   x86_fpu_cmp(cpu_t *c, int i, int pop2);
void   x86_fpu_cmp_mem(cpu_t *c, uint32_t ea, int bits, int pop);
uint16_t x86_fpu_sw(cpu_t *c);
void   x86_fldcw(cpu_t *c, uint16_t cw);
void   x86_fstenv(cpu_t *c, uint32_t ea);
void   x86_fldenv(cpu_t *c, uint32_t ea);
void   x86_fsave(cpu_t *c, uint32_t ea);
void   x86_frstor(cpu_t *c, uint32_t ea);
void   x86_fsin(cpu_t *c);
void   x86_fcos(cpu_t *c);
void   x86_fptan(cpu_t *c);
void   x86_fpatan(cpu_t *c);
void   x86_fyl2x(cpu_t *c);
void   x86_f2xm1(cpu_t *c);
void   x86_frndint(cpu_t *c);
void   x86_fscale(cpu_t *c);
void   x86_fprem(cpu_t *c);
void   x86_fxam(cpu_t *c);
void   x86_fxtract(cpu_t *c);

/* ---------------------------------------------------------------- SSE */
uint64_t x86_xmm_get(cpu_t *c, int r, int size);
void     x86_xmm_set(cpu_t *c, int r, int size, uint64_t v);
void     x86_xmm_zero(cpu_t *c, int r);
void     x86_xmm_copy(cpu_t *c, int d, int s, int size);
void     x86_sse_copy(cpu_t *c, int d, int s, int size);
uint64_t x86_sse_load(cpu_t *c, uint32_t ea, int size);
void     x86_sse_store_xmm(cpu_t *c, int r, uint32_t ea, int size);
void     x86_sse_load_xmm(cpu_t *c, int r, uint32_t ea, int size);
uint64_t x86_xmm_get64(cpu_t *c, int r, int half);
void     x86_xmm_set64(cpu_t *c, int r, int half, uint64_t v);
uint32_t x86_xmm_lo32(cpu_t *c, int r);
void     x86_xmm_set_lo32(cpu_t *c, int r, uint32_t v);
void     x86_sse_setop(cpu_t *c, int r, int size, uint64_t a, uint64_t b, int op);
void     x86_sse_intop(cpu_t *c, int r, uint64_t v, int op);
void     x86_sse_comis(cpu_t *c, uint64_t a, uint64_t b);
void     x86_sse_unpck(cpu_t *c, int r, uint64_t v, int high, int dwords);
void     x86_shufpd(cpu_t *c, int r, uint64_t v, int imm);
void     x86_pshufd(cpu_t *c, int r, int imm, uint64_t src);
void     x86_psrlq(cpu_t *c, int r, int cnt);
void     x86_psllq(cpu_t *c, int r, int cnt);
void     x86_psrld(cpu_t *c, int r, int cnt);
void     x86_pslld(cpu_t *c, int r, int cnt);
void     x86_psraw(cpu_t *c, int r, int cnt);
void     x86_psrad(cpu_t *c, int r, int cnt);
void     x86_psrlw(cpu_t *c, int r, int cnt);
void     x86_psllw(cpu_t *c, int r, int cnt);
uint32_t x86_pmovmskb(cpu_t *c, int r);
uint32_t x86_pextrw(cpu_t *c, int r, int idx);
void     x86_pinsrw(cpu_t *c, int r, int idx, uint64_t v);
void     x86_pmaxsw(cpu_t *c, int r, uint64_t v);
void     x86_pminsw(cpu_t *c, int r, uint64_t v);
void     x86_pmulhw(cpu_t *c, int r, uint64_t v);
void     x86_pmaddwd(cpu_t *c, int r, uint64_t v);
double   x86_i2d(uint32_t v);
uint32_t x86_d2i(uint64_t bits);
double   x86_sqrt(uint64_t bits);

/* Call a recompiled or native function from native code (stdcall).  `target`
 * is the function address, `args` includes `this` as args[0]. */
uint32_t x86_call_stdcall(uint32_t target, const uint32_t *args, int nargs);

/* ------------------------------------------------------------------ misc */
extern const char *x86_game_dir;

#ifdef __cplusplus
}
#endif
#endif /* X86RT_H */
