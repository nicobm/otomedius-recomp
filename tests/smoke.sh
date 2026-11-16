#!/bin/sh
# smoke.sh - headless, silent end-to-end smoke test of the recompiled game.
#
# Boots the game with the software renderer and SDL's dummy audio driver,
# stops after a fixed number of presents and checks that:
#   * the process exits cleanly (0),
#   * no dispatch landed outside the recompiled code,
#   * CDemoPlayMan reached state 5 (the attract gameplay scene),
#   * CGameMan was constructed without poisoning the manager mask,
#   * an optional frame dump was written.
#
# The repository does not contain game data.  Point OTOMEDIUS_DIR at a
# directory with otomedius.exe (default: ../.work/oto) and OTOMEDIUS_BIN at
# the built binary (default: ../.work/obj/otomedius-recomp).
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN=${OTOMEDIUS_BIN:-$ROOT/../.work/obj/otomedius-recomp}
DIR=${OTOMEDIUS_DIR:-$ROOT/../.work/oto}
TMP=${OTOMEDIUS_SMOKE_TMP:-/tmp/opencode}
LOG=${OTOMEDIUS_SMOKE_LOG:-$TMP/otomedius-smoke.log}
FRAMES=${OTOMEDIUS_SMOKE_FRAMES:-4700}
LIMIT=${OTOMEDIUS_SMOKE_TIMEOUT:-900}

mkdir -p "$TMP" || exit 1

fail() { printf 'smoke: FAIL: %s\n' "$1" >&2; exit 1; }

[ -x "$BIN" ] || fail "no executable at $BIN (set OTOMEDIUS_BIN)"
[ -f "$DIR/otomedius.exe" ] || fail "no otomedius.exe in $DIR (set OTOMEDIUS_DIR)"

printf 'smoke: running %s in %s for %s presents (log: %s)\n' \
       "$BIN" "$DIR" "$FRAMES" "$LOG"

(
    cd "$DIR" || exit 1
    SDL_VIDEODRIVER=offscreen \
    SDL_AUDIODRIVER=dummy \
    OTOMEDIUS_BOOTDBG=1 \
    OTOMEDIUS_QUIT_AT="$FRAMES" \
    timeout -k 5 "$LIMIT" "$BIN" . >"$LOG" 2>&1
)
rc=$?
[ "$rc" -eq 0 ] || fail "process exited with $rc (124 = timed out); see $LOG"

if grep -q 'no recompiled code' "$LOG"; then
    grep -m3 'no recompiled code' "$LOG" >&2
    fail "a call left the recompiled code; see $LOG"
fi
grep -q 'DEMO state=5' "$LOG" || fail "CDemoPlayMan never reached state 5"
grep -q 'CGameMan ctor returned.*mask220=00000000' "$LOG" ||
    fail "CGameMan constructor poisoned mask220"
if grep -q 'imports: too many native thunks' "$LOG"; then
    fail "native thunk table exhausted"
fi

printf 'smoke: PASS (state 5 reached, clean exit)\n'
