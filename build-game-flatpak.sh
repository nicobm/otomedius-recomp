#!/usr/bin/env bash
# Build and install Otomedius (recompiled) as a Flatpak.
#
#   ./build-game-flatpak.sh [path/to/game.zip]
#
# The script is distribution agnostic: on a machine without flatpak or
# flatpak-builder it prints the package names for the distribution it detects.
# Everything else (runtime, SDK, Python wheels, the recompiled game) is
# handled here.  The Flatpak build itself runs without network access and the
# installed application can only see Wayland/X11, audio, the GPU and input
# devices: no home directory, no host filesystem, no network.
set -euo pipefail

cd "$(dirname "$0")"

APP_ID="io.github.nicobm.OtomediusRecomp"
MANIFEST="${APP_ID}.json"
RUNTIME_REF="org.freedesktop.Platform//26.08"
SDK_REF="org.freedesktop.Sdk//26.08"
GAME_ZIP_SHA256="4c6562eab3fff1bd1a6e84732bcb565a871440fcb894985fe990fe566a8bd897"

say()  { printf '\033[1m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 1. Check the tooling and explain how to install it on this distribution.
# ---------------------------------------------------------------------------
detect_distro() {
    if [ -r /etc/os-release ]; then
        # shellcheck disable=SC1091
        . /etc/os-release
        echo "${ID:-linux} ${ID_LIKE:-}"
    elif command -v lsb_release >/dev/null 2>&1; then
        lsb_release -si | tr '[:upper:]' '[:lower:]'
    else
        echo linux
    fi
}

install_hint() {
    local distro="$1" missing="$2"
    case "$distro" in
        *fedora*|*rhel*|*centos*|*silverblue*|*kinoite*)
            echo "  sudo dnf install $missing"
            if [ -n "${VARIANT_ID:-}" ] && [ "$VARIANT_ID" = silverblue ]; then
                echo "  (Silverblue/Kinoite: flatpak is preinstalled, flatpak-builder"
                echo "   needs an overlay:  rpm-ostree install flatpak-builder && reboot)"
            fi
            ;;
        *debian*|*ubuntu*|*linuxmint*|*pop*)
            echo "  sudo apt update && sudo apt install $missing"
            ;;
        *arch*|*manjaro*|*endeavouros*)
            echo "  sudo pacman -S $missing"
            ;;
        *opensuse*|*suse*)
            echo "  sudo zypper install $missing"
            ;;
        *alpine*)
            echo "  sudo apk add flatpak flatpak-builder"
            ;;
        *void*)
            echo "  sudo xbps-install -S flatpak flatpak-builder"
            ;;
        *)
            echo "  install the packages '$missing' with your distribution package manager"
            ;;
    esac
}

DISTRO="$(detect_distro)"
MISSING=""
command -v flatpak >/dev/null 2>&1 || MISSING="$MISSING flatpak"
command -v flatpak-builder >/dev/null 2>&1 || MISSING="$MISSING flatpak-builder"
command -v python3 >/dev/null 2>&1 || MISSING="$MISSING python3"
command -v unzip >/dev/null 2>&1 || MISSING="$MISSING unzip"
command -v curl >/dev/null 2>&1 || command -v wget >/dev/null 2>&1 || MISSING="$MISSING curl"

if [ -n "$MISSING" ]; then
    echo "This build needs:${MISSING}" >&2
    echo "Install them with:" >&2
    install_hint "$DISTRO" "$MISSING"
    die "missing build tools"
fi

# ---------------------------------------------------------------------------
# 2. Find the user's own copy of the game and check its hash.
# ---------------------------------------------------------------------------
if [ $# -ge 1 ]; then
    GAME_ZIP="$1"
elif [ -f game.zip ]; then
    GAME_ZIP="game.zip"
else
    GAME_ZIP=""
    while IFS= read -r candidate; do
        if [ -f "$candidate" ]; then
            GAME_ZIP="$candidate"
            break
        fi
    done < <(ls -1 ./*.zip 2>/dev/null || true)
fi

[ -n "$GAME_ZIP" ] && [ -f "$GAME_ZIP" ] || die \
"no game archive found.

Put your own copy of the Otomedius arcade dump (the .zip used by TeknoParrot)
in this directory, or pass its path:

    ./build-game-flatpak.sh /path/to/otomedius.zip

This project does not distribute the game in any form."

say "checking $GAME_ZIP"
SUM="$(sha256sum "$GAME_ZIP" | cut -d' ' -f1)"
if [ "$SUM" != "$GAME_ZIP_SHA256" ]; then
    warn "the archive does not match the known Otomedius dump"
    warn "  expected sha256: $GAME_ZIP_SHA256"
    warn "  your file:       $SUM"
    warn "the build will continue, but the recompiled code is only tested"
    warn "against that exact executable (recomp/exe.sha256)"
fi

mkdir -p flatpak/wheels
[ "$(realpath "$GAME_ZIP")" = "$(realpath game.zip 2>/dev/null || echo "$PWD/game.zip")" ] || \
    cp -f "$GAME_ZIP" game.zip

python3 - <<'PY' || die "the game archive does not contain otomedius.exe"
import sys, zipfile
with zipfile.ZipFile("game.zip") as z:
    names = {n.lower(): n for n in z.namelist()}
if "otomedius.exe" not in names:
    sys.exit(1)
PY

# ---------------------------------------------------------------------------
# 3. Runtime and SDK (26.08 is the current Freedesktop release).
# ---------------------------------------------------------------------------
flatpak_ref_installed() { flatpak info "$1" >/dev/null 2>&1; }
flatpak_has_remote() { flatpak remotes "$1" --columns=name 2>/dev/null | grep -qx "$2"; }

install_ref() {
    local ref="$1"
    flatpak_ref_installed "$ref" && return 0
    local remote
    for remote in flathub flathub-verified_floss flathub-floss fedora; do
        if flatpak_has_remote --user "$remote"; then
            say "installing $ref from $remote (user)"
            flatpak install -y --user "$remote" "$ref" >/dev/null 2>&1 && \
                flatpak_ref_installed "$ref" && return 0
        fi
    done
    for remote in flathub flathub-verified_floss flathub-floss fedora; do
        if flatpak_has_remote --system "$remote"; then
            say "installing $ref from $remote (system)"
            flatpak install -y "$remote" "$ref" >/dev/null 2>&1 && \
                flatpak_ref_installed "$ref" && return 0
        fi
    done
    if ! flatpak_has_remote --user flathub; then
        say "adding the Flathub remote"
        flatpak remote-add --if-not-exists --user flathub \
            https://flathub.org/repo/flathub.flatpakrepo >/dev/null 2>&1 || true
    fi
    if flatpak_has_remote --user flathub; then
        say "installing $ref from flathub (user)"
        flatpak install -y --user flathub "$ref" >/dev/null 2>&1 && \
            flatpak_ref_installed "$ref" && return 0
    fi
    echo "Could not install $ref automatically." >&2
    echo "Run: flatpak install -y flathub $RUNTIME_REF $SDK_REF" >&2
    return 1
}

install_ref "$RUNTIME_REF" || die "missing runtime"
install_ref "$SDK_REF" || die "missing SDK"

# ---------------------------------------------------------------------------
# 4. Python build dependency (the Flatpak build has no network).
# ---------------------------------------------------------------------------
CAPSTONE="capstone-5.0.9-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl"
CAPSTONE_SHA256="273fd8d747d2e35c88f91450be51a603ecfaafb00d96d9f315dcb8689c86193e"
CAPSTONE_URL="https://files.pythonhosted.org/packages/d1/39/17747862222bb062e86b501f1f148d5ff589b77908b080d30f7f085cbfb7/$CAPSTONE"

check_sha() { [ -f "$1" ] && [ "$(sha256sum "$1" | cut -d' ' -f1)" = "$2" ]; }

if ! check_sha "flatpak/wheels/$CAPSTONE" "$CAPSTONE_SHA256"; then
    say "downloading $CAPSTONE"
    rm -f "flatpak/wheels/$CAPSTONE" "flatpak/wheels/$CAPSTONE.part"
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 3 -o "flatpak/wheels/$CAPSTONE.part" "$CAPSTONE_URL"
    else
        wget -O "flatpak/wheels/$CAPSTONE.part" "$CAPSTONE_URL"
    fi
    mv -f "flatpak/wheels/$CAPSTONE.part" "flatpak/wheels/$CAPSTONE"
    check_sha "flatpak/wheels/$CAPSTONE" "$CAPSTONE_SHA256" || die "bad capstone download"
fi

# ---------------------------------------------------------------------------
# 5. Build and install for the current user.
# ---------------------------------------------------------------------------
flatpak remote-delete --user recomp-origin >/dev/null 2>&1 || true
flatpak remote-delete --user debug-origin >/dev/null 2>&1 || true

say "building $APP_ID (this takes a few minutes)"
exec flatpak-builder --user --install --force-clean --disable-rofiles-fuse \
    --repo=repo build-dir "$MANIFEST"
