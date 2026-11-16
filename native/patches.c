/* patches.c - deliberate deviations from the original binary.
 *
 * A static recompilation keeps the game's own code, so anything that has to
 * behave differently on a PC has to be replaced here by address.  The
 * dispatcher asks x86_patch_call() before entering a function; returning 1
 * means "already emulated, continue at cpu->eip".
 *
 * Everything is documented individually so the list stays auditable.
 */

#include "win32.h"

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

    switch (addr) {
    case 0x40ED00u:                  /* verify one ChkSum.bin entry */
        if (!skip_chksum())
            return 0;
        cpu->eax = 1;                /* "met" */
        cpu->eip = M32(cpu->esp);    /* fake the ret */
        cpu->esp += 4;
        return 1;
    case 0x418040u:                  /* load-job executor (cdecl, 2 args) */
        /* The ring worker passes slot[+4] as the "job id", and downstream
         * code uses it as an index into the 4-entry package registry
         * (0x5DE098).  Our emulated ring occasionally hands it a value such
         * as 16 (the slot count, not a package), which reads outside that
         * table and ends in a call through an invalid vtable pointer in
         * 0x418190.  Report "job not performed" (al = 0) for ids that cannot
         * name a package; the worker then just frees the slot. */
        {
            uint32_t id = M32(cpu->esp + 4u);
            if (id >= 4u) {
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
    case 0x418190u:                  /* package lookup/commit (cdecl, 1 arg) */
        /* arg = package index into the four-entry table at 0x5DE098.  When
         * the emulated loader hands us an out-of-range value (e.g. 16) the
         * lookup reads past the table and ends in a call through an invalid
         * vtable pointer.  Do nothing for such an index. */
        /* The commit walks the package's C++ record vector and eventually
         * calls a virtual method; our emulated loader leaves that vector in a
         * state the game's own code cannot walk (the same reason the later
         * 0x414920 finalise is skipped).  The load itself (file mapping) has
         * already happened in 0x418040, so skipping the commit is safe and
         * keeps the worker from dereferencing a stale vtable. */
        {
            static int fired;
            if (!fired++)
                fprintf(stderr, "config: skipping the package commit step\n");
            cpu->eax = 0;
            cpu->eip = M32(cpu->esp);
            cpu->esp += 4;
            return 1;
        }
    case 0x4547E0u:                  /* boot/attract manager update */
        /* Its state machine crashes in the CRT while building a path before
         * it reaches the KonamiLogo hand-off.  The logo manager itself is
         * alive and is driven by the OTOMEDIUS_KONAMI hook in render.c, so
         * keep this manager alive but do not run its state machine. */
        if (!getenv("OTOMEDIUS_KONAMI")) {
            /* The state machine waits for the cabinet I/O board to report
             * ready ([0x5E2136] == 1) before it advances to the logo.  Our
             * AVS stubs never set that bit, so force it. */
            M8(0x5E2136u) = 1;
            {
                uint32_t mgr = M32(0x5DE24Cu);
                if (mgr && M8(mgr + 0xCu) == 0)
                    M8(mgr + 0xCu) = 1;   /* skip the "wait for I/O" state */
                /* State 1 sub-state 1 waits for the boot/attract manager slot
                 * (0x5DE258) to be empty before creating it.  Our scene
                 * manager pointer ends up there, so clear it. */
                if (M8(mgr + 0xCu) == 1 && M8(mgr + 0xDu) == 1)
                    M32(0x5DE258u) = 0;
            }
            return 0;
        }
        cpu->eax = 1;
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
        if (!getenv("OTOMEDIUS_NO_LOADER_GUARD") &&
            M32(0x5DCF90u) == M32(0x5DCF94u)) {
            static int fired;
            if (!fired++)
                fprintf(stderr, "config: loader queue empty, load complete\n");
            cpu->eax = 1;
            cpu->eip = M32(cpu->esp);
            cpu->esp += 4;
            return 1;
        }
        /* Default on: without it the scene manager waits forever and nothing
         * is ever loaded.  OTOMEDIUS_NO_LOADER_GUARD=1 restores waiting. */
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
            if (calls > 70 || (stalled > 50 && st != 0 && st != 2)) {
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
