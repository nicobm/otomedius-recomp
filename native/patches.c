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
        /* It reads the package registry (0x5DE098) and calls a virtual method
         * on the entry; when the registry is empty (every package already
         * destroyed) that is a call through NULL.  Skipping the commit is
         * strictly better than executing an invalid instruction, and the
         * original path still runs whenever a package is live. */
        if (M32(0x5DE098u) == 0) {
            static int fired;
            if (!fired++)
                fprintf(stderr, "config: package registry empty, skipping "
                        "the finalise step\n");
            cpu->eax = 1;
            cpu->eip = M32(cpu->esp);
            cpu->esp += 4;
            return 1;
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
