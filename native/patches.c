/* patches.c - deliberate deviations from the original binary.
 *
 * A static recompilation keeps the game's own code, so anything that has to
 * behave differently on a PC has to be replaced here by address.  The
 * dispatcher asks x86_patch_call() before entering a function; returning 1
 * means "already emulated, continue at cpu->eip".
 *
 * Everything is documented individually so the list stays auditable.
 */

#define _GNU_SOURCE
unsigned render_presents(void);
#include "win32.h"
#include "d3d9.h"

#include <time.h>
#include <string.h>

/* 0x4ecbf3 - CPlayer02::Init waits for its motion/animation interface.
 *
 * The interface comes from the .x skeletal animation that
 * D3DXLoadMeshHierarchyFromXInMemory would build; this port does not parse .x
 * files yet, so the field at scene+0x150 is NULL and the original code
 * dereferences address 0.  When the field is empty, install a tiny COM object
 * whose methods do nothing so the attract can keep running (the player model
 * will be missing, but the rest of the scene/state machine advances). */
static void model_stub(void) { }
static uint32_t fake_model_iface(void)
{
    static uint32_t obj;
    if (!obj) {
        static const method_t m[4] = {
            {model_stub, 3}, {model_stub, 3}, {model_stub, 3}, {model_stub, 3}
        };
        obj = com_create(m, 4, 0x50);
        M32(obj + 0x44) = obj + 4;   /* the game expects [field+0x44] */
    }
    return obj;
}

/* 0x40ED00 - file integrity check.
 *
 * `ChkSum.bin` holds 191 file names and their MD5, and the boot path verifies
 * every one of them in order, aborting on the first mismatch (it then logs
 * event 4 through 0x405120 and returns 0, which destroys the loading manager).
 * The dump this port runs against is a later revision: 22 of the listed files
 * - starting with Data/CardEntry.pak - have different contents, so the check
 * fails and the cabinet never leaves the asset loader.
 *
 * On real hardware that check is an anti-tamper gate; on a preservation port it
 * is exactly what blocks a legitimately different build from booting, so it is
 * skipped.  Set OTOMEDIUS_CHKSUM=1 to keep the original behaviour when running
 * against a dump that matches the manifest byte for byte.
 */
static int skip_chksum(void)
{
    const char *v = getenv("OTOMEDIUS_CHKSUM");
    return !(v && *v && *v != '0');
}

int x86_patch_call(uint32_t addr)
{
    cpu_t *cpu = x86_cpu();

    /* See fake_model_iface(): keep CPlayer02::Init from dereferencing the
     * missing .x animation interface. */
    if (addr == 0x4ecbf3u) {
        uint32_t self = cpu->esi;
        uint32_t a = self ? M32(self + 0x94u) : 0;
        uint32_t b = a ? M32(a + 8u) : 0;
        if (b && !M32(b + 0x150u)) {
            M32(b + 0x150u) = fake_model_iface();
            static int fired;
            if (!fired++)
                fprintf(stderr, "config: no .x animation for this scene, "
                        "using an empty motion object\n");
        }
    }

    /* OTOMEDIUS_BOOTDBG=1 traces the boot/attract hand-off: the state and
     * sub-state of the manager, the local-heap allocator and the manager
     * constructor that registers in slot 0x5DE258. */
    if (getenv("OTOMEDIUS_BOOTDBG")) {
        if (addr == 0x4547E0u) {
            uint32_t mgr = M32(0x5DE24Cu);
            static unsigned calls;
            static int last_state = -1, last_sub = -1;
            if (mgr) {
                int st = M8(mgr + 0xCu), sb = M8(mgr + 0xDu);
                if (st != last_state || sb != last_sub) {
                    fprintf(stderr, "bootdbg: [present %u] update #%u state=%u sub=%u io=%u "
                            "slot258=%08x mask220=%08x mask224=%08x\n", render_presents(), calls,
                            st, sb, M8(0x5E2136u),
                            M32(0x5DE258u), M32(0x5DE220u), M32(0x5DE224u));
                    last_state = st;
                    last_sub = sb;
                }
                calls++;
            }
        } else if (addr == 0x419170u || addr == 0x419330u || addr == 0x4193A0u ||
                   addr == 0x418040u || addr == 0x419D00u || addr == 0x418190u) {
            fprintf(stderr, "bootdbg: [present %u] LOADER %05x esi=%x ebx=%x arg0=%x "
                    "h=%x curid=%x state=%u head=%u tail=%u\n",
                    render_presents(), (unsigned)addr, cpu->esi, cpu->ebx,
                    M32(cpu->esp + 4u), M32(0x5BC914u), M32(0x5BC918u),
                    M32(0x5BC91Cu), M32(0x5DCF90u), M32(0x5DCF94u));
        } else if (addr == 0x4142B0u) {
            uint32_t desc = M32(cpu->esp + 4u), size = M32(cpu->esp + 8u);
            static unsigned nmalloc;
            if (getenv("OTOMEDIUS_MALLOCDBG") && nmalloc++ < 400)
                fprintf(stderr, "bootdbg: MallocLocal(%u) desc=%08x first=%08x "
                        "name=%08x\n", size, desc, desc ? M32(desc + 4u) : 0,
                        desc ? M32(desc) : 0);
        } else if (addr == 0x429210u) {
            fprintf(stderr, "bootdbg: manager ctor this=%08x slot258=%08x\n",
                    cpu->esi, M32(0x5DE258u));
        } else if (addr == 0x429340u) {
            /* CDemoPlayMan::Update: state in this+0xA; waits for the replay
             * pak loader, the eye-catch manager (0x5DE25C) and creates
             * CGameMan in 0x5DE260. */
            uint32_t self = cpu->ecx;
            static int last = -1;
            static unsigned present_last;
            int st = self ? M8(self + 0xAu) : -1;
            unsigned p = render_presents();
            if (st != last || p - present_last >= 120u) {
                fprintf(stderr, "bootdbg: [present %u] DEMO state=%d "
                        "slot25c=%08x slot260=%08x io=%u mask220=%08x "
                        "slot258=%08x\n",
                        p, st, M32(0x5DE25Cu),
                        M32(0x5DE260u), M8(0x5E2136u), M32(0x5DE220u),
                        M32(0x5DE258u));
                last = st;
                present_last = p;
            }
        } else if (addr == 0x42E1F0u) {
            /* CGameMan constructor called from CDemoPlayMan state 4. */
            uint32_t self = M32(cpu->esp + 4u);
            static unsigned n;
            fprintf(stderr, "bootdbg: [present %u] CGameMan ctor #%u this=%08x "
                    "slot260=%08x mask220=%08x\n", render_presents(), ++n,
                    self, M32(0x5DE260u), M32(0x5DE220u));
        } else if (addr == 0x429443u) {
            fprintf(stderr, "bootdbg: [present %u] CGameMan ctor returned, "
                    "eax=%08x slot260=%08x mask220=%08x\n",
                    render_presents(), cpu->eax, M32(0x5DE260u),
                    M32(0x5DE220u));
        } else if (addr == 0x4095a0u) {
            /* Generic job-list runner (sorts then calls every record).  A bad
             * record here means something overwrote the list or its count. */
            uint32_t self = cpu->esi;
            unsigned n = self ? M32(self + 0x10u) : 0;
            uint32_t b = self ? M32(self + 8u) : 0;
            int bad = n > 16;
            for (unsigned i = 0; !bad && i < n; i++) {
                uint32_t fn = M32(b + i * 12u + 4u);
                if (fn && (fn < 0x400000u || fn >= 0x5F0000u))
                    bad = 1;
            }
            if (bad) {
                fprintf(stderr, "bootdbg: [present %u] BAD RUNLIST self=%08x "
                        "base=%08x cur=%08x n=%u\n", render_presents(), self,
                        b, self ? M32(self + 0xCu) : 0, n);
                for (unsigned i = 0; i < n && i < 24; i++)
                    fprintf(stderr, "    rec[%u] %08x: prio=%08x fn=%08x "
                            "arg=%08x\n", i, b + i * 12u, M32(b + i * 12u),
                            M32(b + i * 12u + 4u), M32(b + i * 12u + 8u));
            }
        } else if (addr == 0x4ecbf3u) {
            uint32_t self = cpu->esi;
            uint32_t a = self ? M32(self + 0x94u) : 0;
            uint32_t b = a ? M32(a + 8u) : 0;
            uint32_t c = b ? M32(b + 0x150u) : 0;
            fprintf(stderr, "bootdbg: [present %u] stage obj self=%08x "
                    "vt=%08x a=%08x b=%08x bvt=%08x c=%08x\n",
                    render_presents(), self, self ? M32(self) : 0, a, b,
                    b ? M32(b) : 0, c);
        } else if (addr == 0x403DB0u) {
            uint32_t msg = M32(cpu->esp + 8u), id = M32(cpu->esp + 12u);
            static int last_id = -1;
            static unsigned nlog;
            if ((int)id != last_id && nlog < 40) {
                char buf[128];
                const unsigned char *p = msg ? (const unsigned char *)(MEM + msg) : 0;
                int i = 0;
                if (p)
                    for (; i < (int)sizeof(buf) - 1 && p[i]; i++)
                        buf[i] = (p[i] >= 32 && p[i] < 127) ? (char)p[i] : '?';
                buf[i] = 0;
                fprintf(stderr, "bootdbg: [present %u] LOG id=%d (0x%x) msg=%08x "
                        "\"%s\" mask220=%08x\n", render_presents(), (int)id, id,
                        msg, msg ? buf : "", M32(0x5DE220u));
                last_id = (int)id;
                nlog++;
            }
        } else if (addr == 0x439550u || addr == 0x439BB0u) {
            uint32_t pkg = M32(0x5DE0BCu);
            static uint32_t last[4];
            uint32_t beg = pkg ? M32(pkg + 4u) : 0;
            uint32_t end = pkg ? M32(pkg + 8u) : 0;
            uint32_t rec = (beg && beg < end) ? M32(beg) : 0;
            uint32_t objs = rec ? M32(rec) : 0;
            uint32_t tex = (rec && objs) ? M32(rec + 4u) : 0;
            if (addr == 0x439BB0u || pkg != last[0] || beg != last[1] ||
                end != last[2] || tex != last[3]) {
                last[0] = pkg; last[1] = beg; last[2] = end; last[3] = tex;
                fprintf(stderr, "bootdbg: [present %u] logo@%x pkg=%08x vec=%08x..%08x "
                        "rec=%08x n=%u tex=%08x\n", render_presents(), (unsigned)addr, pkg, beg, end, rec, objs,
                        tex);
            }
        } else if (addr == 0x417750u) {
            uint32_t pmt = M32(cpu->esp + 4u);
            uint32_t pb = pmt ? M32(pmt + 0x44u) : 0;
            fprintf(stderr, "bootdbg: renderer SetMediaType(pmt=%08x pb=%08x "
                    "%dx%d cb=%u)\n", pmt, pb,
                    pb ? (int)M32(pb + 0x34u) : 0,
                    pb ? (int)M32(pb + 0x38u) : 0,
                    pmt ? M32(pmt + 0x40u) : 0);
        }
    }

    switch (addr) {
    case 0x40ED00u:                  /* verify one ChkSum.bin entry */
        if (!skip_chksum())
            return 0;
        cpu->eax = 1;                /* "met" */
        cpu->eip = M32(cpu->esp);    /* fake the ret */
        cpu->esp += 4;
        return 1;
    case 0x418040u:                  /* load-job executor (cdecl, 2 args) */
        /* The ring stores one asset/package id per slot (0x419170 writes it
         * at slot+4 and 0x419D00 passes it here).  0x4186F0 uses that id as
         * an index into the 16-entry package table at 0x5DE098 (the same
         * count as the ring slots at 0x5DCF98).  The intro logos request
         * ids such as 9, so only reject ids that fall outside the table. */
        {
            uint32_t id = M32(cpu->esp + 4u);
            if (id >= 16u) {
                static int fired;
                if (!fired++)
                    fprintf(stderr, "config: loader job id %u out of range, "
                            "skipping\n", id);
                cpu->eax = 0;
                cpu->eip = M32(cpu->esp);
                cpu->esp += 4;
                return 1;
            }
        }
        return 0;
    case 0x4186F0u:                  /* pak parse (cdecl: id, header/name, ...) */
        /* Before a new package-9 asset is parsed, drop the pointer to the
         * previous scene's group (the block itself is leaked on purpose; the
         * vector keeps its capacity so the game's append path is unchanged).
         * Sound banks ("SDBK") share the id and must not clear it. */
        if (M32(cpu->esp + 4u) == 9u) {
            uint32_t hdr = M32(cpu->esp + 8u);
            uint32_t pkg = M32(0x5DE0BCu);
            int snd = hdr && !memcmp(MEM + hdr, "SDBK", 4);
            if (!snd && pkg && M32(pkg + 4u))
                M32(pkg + 8u) = M32(pkg + 4u);
        }
        return 0;
    case 0x418190u:                  /* package lookup/commit (cdecl, 1 arg) */
        /* With the correct id range the package vector is the one the loader
         * has just filled (0x4185B0/0x4186F0), so let the original commit run
         * and register the loaded objects in the game's asset tables.  Some
         * jobs (Title.pak) leave a record whose object/vtable is NULL; the
         * release loop at 0x418228 then does `call [[obj]+8]` through NULL.
         * Detect that shape first and skip only those commits. */
        {
            uint32_t id = M32(cpu->esp + 4u);
            /* Package 9 is the shared "boot/attract" asset package: every
             * logo/warning/title load appends a texture group and the commit
             * at the end of the job would free it again before the manager
             * draws (KonamiLogo.pak and KonamiLogoSnd.pak even run as two
             * concurrent jobs on the same id, and the sound job's commit
             * wiped the logo group).  Keep the group alive; the next load
             * for id 9 drops the previous one at 0x4186F0. */
            if (id == 9u) {
                cpu->eax = 0;
                cpu->eip = M32(cpu->esp);
                cpu->esp += 4;
                return 1;
            }
            uint32_t pkg = id < 16u ? M32(0x5DE098u + id * 4u) : 0;
            uint32_t beg = pkg ? M32(pkg + 4u) : 0;
            uint32_t end = pkg ? M32(pkg + 8u) : 0;
            int bad = 0;
            for (uint32_t q = beg; q && q + 4u <= end && !bad; q += 4u) {
                uint32_t grp = M32(q);
                uint32_t n = grp ? M32(grp) : 0;
                if (!grp || n > 4096u) {
                    bad = 1;
                    break;
                }
                for (uint32_t i = 0; i < n; i++) {
                    uint32_t obj = M32(grp + 4u + i * 0x44u);
                    if (!obj || !M32(obj)) {
                        bad = 1;
                        break;
                    }
                }
            }
            if (bad) {
                static int fired;
                if (!fired++)
                    fprintf(stderr, "config: package %u has a dead record, "
                            "skipping the commit step\n", id);
                cpu->eax = 0;
                cpu->eip = M32(cpu->esp);
                cpu->esp += 4;
                return 1;
            }
        }
        return 0;
    case 0x4547E0u:                  /* boot/attract manager update */
        /* OTOMEDIUS_KONAMI=1 skips the real state machine and lets the
         * OTOMEDIUS_KONAMI hook in render.c drive the logo manager directly
         * (kept as a diagnostic).  Otherwise run the original state machine:
         * its state 0 creates the pre-logo managers in sequence (KonamiLogo,
         * eAMUSELogo, Warning, intro movie), state 1 creates the title scene
         * and the later states handle card entry / the main game. */
        if (!getenv("OTOMEDIUS_KONAMI"))
            return 0;
        cpu->eax = 1;
        cpu->eip = M32(cpu->esp);
        cpu->esp += 4;
        return 1;
    case 0x4050B0u:                  /* release the two sound/texture slots */
        /* The shutdown path releases the objects in 0x5E1F88 / 0x5E1F8C and
         * then frees the holders.  When the intro movie is interrupted with
         * Esc, the COM object behind one holder can already have a zeroed
         * vtable, and the game calls through NULL (the "no recompiled code at
         * 00000000" crash after the video).  Do the same teardown here, but
         * tolerate a dead object. */
        {
            static const uint32_t slots[2] = { 0x5E1F88u, 0x5E1F8Cu };
            for (int i = 0; i < 2; i++) {
                uint32_t holder = M32(slots[i]);
                if (!holder)
                    continue;
                uint32_t com = M32(holder);
                if (com) {
                    uint32_t vt = M32(com);
                    if (!vt) {
                        static int fired;
                        if (!fired++)
                            fprintf(stderr, "config: sound object %08x has no "
                                    "vtable, skipping release\n", com);
                    } else {
                        uint32_t rel = M32(vt + 8u);
                        if (rel)
                            x86_call_stdcall(rel, &com, 1);
                    }
                    M32(holder) = 0;
                }
                x86_call_stdcall(0x414460u, &holder, 1);   /* game free */
                M32(slots[i]) = 0;
            }
        }
        cpu->eax = 0;
        cpu->eip = M32(cpu->esp);
        cpu->esp += 4;
        return 1;
    case 0x402840u:                  /* save bookkeep.bin */
        /* The save path builds "E:\nvmem\bookkeep.bin" and then converts it to
         * a host path.  The conversion corrupts the name in our emulation and
         * the following CreateFileA gets a garbage string, which ends in an
         * unhandled exception before the boot sequence can show the logo.
         * Bookkeeping is not needed to reach the logo, so skip the save. */
        {
            static int fired;
            if (!fired++)
                fprintf(stderr, "config: skipping the bookkeep save\n");
            cpu->eax = 0;
            cpu->eip = M32(cpu->esp);
            cpu->esp += 4;
            return 1;
        }
    case 0x403DB0u:                  /* log an error and suspend all managers */
        /* The asset loader compiles its shaders with D3DXCreateEffect.  We do
         * not implement the effect compiler, so it returns E_FAIL and the game
         * logs "2302:Failed Create Shader" through this function, which also
         * sets the manager enable mask (0x5DE220) to 0xFFFFFFFF.  That
         * suspends every manager, including the KonamiLogo one, so the boot
         * never shows.  The shader is only used by the 3D scene renderer; the
         * logo path uses the font atlas, so ignore this particular error and
         * let the managers keep running. */
        {
            uint32_t fmt = M32(cpu->esp + 8u);
            if (fmt == 0x559580u || fmt == 0x5594C4u) {
                static int fired;
                if (!fired++)
                    fprintf(stderr, "config: ignoring the shader compile "
                            "failure (D3DXCreateEffect is a stub)\n");
                cpu->eax = 0;
                cpu->eip = M32(cpu->esp);
                cpu->esp += 4;
                return 1;
            }
        }
        return 0;
    case 0x41C620u:                  /* 2D quad setup (source rect -> vertices) */
        /* The movie callback (0x43F540) computes its source rectangle with an
         * x87 sequence that divides by zero twice: it evaluates 0/scale and
         * then scale2/0, so one of the four coordinates is +inf.  The result
         * is the degenerate quad (1,inf)-(2561,144) documented in §7.2.
         * Clamp the non-finite entries to 0 before the game builds its
         * vertices, so the game's own draw produces a sane rectangle. */
        {
            uint32_t s = cpu->eax;
            if (s) {
                float *f = (float *)(MEM + s + 0x10u);
                int fixed = 0;
                for (int i = 0; i < 4; i++) {
                    if (!__builtin_isfinite(f[i])) {
                        f[i] = 0.0f;
                        fixed = 1;
                    }
                }
                if (fixed) {
                    static int fired;
                    if (g_verbose && !fired++)
                        fprintf(stderr, "config: movie quad had a non-finite "
                                "coordinate, clamped to 0\n");
                }
            }
        }
        return 0;
    case 0x43F370u:                  /* movie player manager update */
        /* State 3 counts 0xDF2 (3570) updates, i.e. 59.5 s at the arcade's
         * 60 Hz.  This port runs the 2D software renderer plus the game's
         * sound mixer at ~34 fps, so the counter would take ~107 s and the
         * already-finished intro would stay on its last frame for another
         * ~47 s.  Feed the counter the elapsed wall-clock ticks (in 1/60 s)
         * so the movie closes in step with the 60 s WMV.  Clamp to 0xDF1 so
         * the manager's own "+1 / cmp 0xDF2" still fires exactly. */
        if (cpu->ecx && M8(cpu->ecx + 0xAu) == 3) {
            static uint64_t last_ms;
            struct timespec ts;
            uint64_t now;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            now = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
            if (last_ms && now > last_ms) {
                uint32_t ticks = (uint32_t)(((now - last_ms) * 60u + 500u) / 1000u);
                if (ticks > 1) {
                    uint32_t t = (uint32_t)M16(cpu->ecx + 8u) + ticks - 1u;
                    if (t >= 0xDF2u)
                        t = 0xDF1u;
                    M16(cpu->ecx + 8u) = (uint16_t)t;
                }
            }
            last_ms = now;
        }
        return 0;
    case 0x419E30u:                  /* "may the job executor run?" gate */
        /* The executor (0x418040) only proceeds when this returns non-zero,
         * and this returns 1 only while a job thread exists *and*
         * [0x5BC91C] == 3.  Nothing in our run ever puts the state at 3, so
         * the work never happens and the ring never drains.  Experiment:
         * drop only the state==3 condition, keep the "a job thread exists"
         * one. */
        if (!getenv("OTOMEDIUS_FORCE_JOB"))
            return 0;
        cpu->eax = M32(0x5bc914u) ? 1u : 0u;
        cpu->eip = M32(cpu->esp);
        cpu->esp += 4;
        return 1;
    case 0x414920u:                  /* commit/finalise the loaded packages */
        /* It calls 0x419690(0) - "first record of package 0" - and then runs
         * [[record]+0x24].  0x419690 returns `[pkg+0x24] + 0x20`, which is
         * NULL whenever the package exists but its record vector was never
         * filled (the normal case in this port, because the async loader is
         * force-completed by the 0x419530 guard).  Both the missing registry
         * and the NULL first record end in a call through address 0, so skip
         * the commit unless there is a real object to commit.  The original
         * path still runs when the loader produced a complete package. */
        {
            uint32_t pkg = M32(0x5DE098u);
            uint32_t beg = pkg ? M32(pkg + 0x24u) : 0;
            uint32_t end = pkg ? M32(pkg + 0x28u) : 0;
            uint32_t obj = 0;
            if (pkg && beg && end >= beg + 0x38u)
                obj = M32(beg + 0x20u);
            if (!obj || !M32(obj)) {
                static int fired;
                if (!fired++)
                    fprintf(stderr, "config: package registry %s, skipping "
                            "the finalise step\n",
                            pkg ? "has no live record" : "empty");
                cpu->eax = 1;
                cpu->eip = M32(cpu->esp);
                cpu->esp += 4;
                return 1;
            }
        }
        return 0;
    case 0x419530u:                  /* "has the async asset load finished?" */
        /* The pump (0x419430) only reports "done" through the state byte, and
         * that requires joining a worker handle whose completion flag never
         * arrives under our thread emulation.  The queue itself is the ground
         * truth: when head == tail every entry has been consumed, which is
         * exactly the condition the pump uses to write state 0. */
        if (getenv("OTOMEDIUS_LOADER_GUARD") &&
            M32(0x5DCF90u) == M32(0x5DCF94u)) {
            static int fired;
            if (!fired++)
                fprintf(stderr, "config: loader queue empty, load complete\n");
            cpu->eax = 1;
            cpu->eip = M32(cpu->esp);
            cpu->esp += 4;
            return 1;
        }
        /* Opt-in since thread_arg_t.done is initialised: the worker now really
         * finishes, so the pump reports completion on its own.
         * OTOMEDIUS_LOADER_GUARD=1 restores the old forced completion. */
        {
            static unsigned stalled, calls;
            static uint32_t last;
            uint32_t st = M32(0x5bc91c);
            calls++;
            if (st == last) {
                stalled++;
            } else {
                stalled = 0;
                last = st;
            }
            if (g_verbose && (calls <= 3 || (calls % 500) == 0))
                fprintf(stderr, "config: loader poll #%u state=%u stalled=%u\n",
                        calls, st, stalled);
            /* ~10 polls/s (the poll itself Sleep(16)), so 70 is ~7 s without
             * the loader making any progress at all. */
            if (getenv("OTOMEDIUS_LOADER_GUARD") &&
                (calls > 70 || (stalled > 50 && st != 0 && st != 2))) {
                static int fired;
                if (!fired++)
                    fprintf(stderr, "config: loader stalled in state %u after "
                            "%u polls, forcing completion\n", st, calls);
                cpu->eax = 1;
                cpu->eip = M32(cpu->esp);
                cpu->esp += 4;
                return 1;
            }
        }
        return 0;
    default:
        return 0;
    }
}
