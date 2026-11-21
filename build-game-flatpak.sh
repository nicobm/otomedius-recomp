#!/usr/bin/env bash
# Build and install Otomedius (recompiled) as a Flatpak.
#
#   ./build-game-flatpak.sh [--debug] [path/to/game.zip]
#
# --debug also installs the <app-id>.Debug runtime (the debug symbols that
# flatpak-builder splits out of the build); it is only useful with gdb and is
# not needed to play, so it is not installed by default.
#
# The script is distribution agnostic: on a machine without flatpak or
# flatpak-builder it prints the package names for the distribution it detects.
# Everything else (runtime, SDK, Python wheels, the recompiled game) is
# handled here.  The Flatpak build itself runs without network access and the
# installed application can only see Wayland/X11, audio, the GPU and input
# devices: no home directory, no host filesystem, no network.
#
# The result is exported to the persistent repo/ directory and installed
# through one local remote, "otomedius-local", which is left disabled: the app
# is built locally, so `flatpak update` has nothing to fetch.  To update it,
# run this script again.
set -euo pipefail

cd "$(dirname "$0")"

APP_ID="io.github.nicobm.OtomediusRecomp"
DEBUG_ID="${APP_ID}.Debug"
LOCAL_REMOTE="otomedius-local"
MANIFEST="${APP_ID}.json"
RUNTIME_REF="org.freedesktop.Platform//26.08"
SDK_REF="org.freedesktop.Sdk//26.08"
GAME_ZIP_SHA256="4c6562eab3fff1bd1a6e84732bcb565a871440fcb894985fe990fe566a8bd897"

# --debug: also install the debug runtime (see the comment at the top).
WITH_DEBUG=0
while [ $# -gt 0 ]; do
    case "$1" in
        --debug) WITH_DEBUG=1; shift ;;
        --) shift; break ;;
        *) break ;;
    esac
done

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
flatpak_has_remote() {
    # `flatpak remotes` hides disabled remotes unless --show-disabled is given,
    # and otomedius-local is disabled between builds; without it, a second run
    # would not find the remote and `remote-add` would fail with
    # "Remote otomedius-local already exists".  Older flatpak has no
    # --show-disabled, so fall back to the plain listing.
    local remotes
    remotes=$(flatpak remotes "$1" --show-disabled --columns=name 2>/dev/null) ||
        remotes=$(flatpak remotes "$1" --columns=name 2>/dev/null) || return 1
    printf '%s\n' "$remotes" | grep -qx "$2"
}

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
# 5. Build, export and install for the current user.
# ---------------------------------------------------------------------------
#
#    `flatpak-builder --install` installs the app through throw-away
#    "<last-id-component>-origin" remotes (otomediusrecomp-origin, debug-origin,
#    and otomediusrecomp1-origin/... when those names are taken).  Those
#    remotes point at the repository of one particular build, so as soon as the
#    checkout moves, or the repository is rebuilt elsewhere, every
#    `flatpak update` prints
#
#      No se encontró la ref. «app/io.github.nicobm.OtomediusRecomp/x86_64/master»
#      en el remoto otomediusrecomp-origin
#
#    and keeps doing it until the remote is deleted.  Deleting the remote
#    alone is not enough either: flatpak then cannot resolve the origin of the
#    installed refs and reports "Remote ... not found".
#
#    Instead, export into the persistent repo/ directory and install from one
#    stable local remote, "otomedius-local".  The remote is left disabled: the
#    app is built locally, so `flatpak update` has nothing to fetch, and a
#    disabled remote cannot fail later (for example if repo/ is deleted).  To
#    update the app, run this script again.

# Origin remotes used by an app or debug runtime installed by an older version
# of this script.  They are deleted after the refs have been reinstalled from
# otomedius-local below, never before: deleting them first would leave
# `flatpak update` with refs whose origin does not exist.
STALE_ORIGINS=""
# `flatpak list --columns=ref` prints refs without the kind prefix
# (io.github.nicobm.OtomediusRecomp/x86_64/master), unlike `flatpak remote-ls`;
# accept both spellings.
while IFS="$(printf '\t') " read -r ref origin; do
    case "$ref" in
        app/"$APP_ID"/*|"$APP_ID"/*|runtime/"$DEBUG_ID"/*|"$DEBUG_ID"/*) ;;
        *) continue ;;
    esac
    case "$origin" in
        ""|-|"$LOCAL_REMOTE") continue ;;
        *-origin) ;;
        *) continue ;;
    esac
    case " $STALE_ORIGINS " in
        *" $origin "*) ;;
        *) STALE_ORIGINS="$STALE_ORIGINS $origin" ;;
    esac
done <<EOF
$(flatpak list --user --all --columns=ref,origin 2>/dev/null || true)
EOF

say "building $APP_ID (this takes a few minutes)"
flatpak-builder --user --force-clean --disable-rofiles-fuse \
    --repo="$PWD/repo" build-dir "$MANIFEST"

# The remote URL is a file:// URI; the checkout path may contain spaces, so
# let Python escape it.
repo_uri=$(python3 -c 'import pathlib, sys
print(pathlib.Path(sys.argv[1]).as_uri())' "$PWD/repo")

# Do not add --no-enumerate: flatpak hides the refs of a no-enumerate remote
# from `flatpak remote-ls` unless it has an xa.main-ref set (see
# flatpak_dir_list_remote_refs), and `flatpak remote-add` does not set one, so
# the remote would be listed but appear empty.  The remote is disabled after
# the install instead, which also keeps it out of app stores and updates.
if flatpak_has_remote --user "$LOCAL_REMOTE"; then
    flatpak remote-modify --user --no-gpg-verify --no-use-for-deps --enumerate \
        --enable --url="$repo_uri" "$LOCAL_REMOTE"
else
    flatpak remote-add --user --no-gpg-verify --no-use-for-deps \
        "$LOCAL_REMOTE" "$repo_uri"
fi

# Install the app, plus the debug runtime only with --debug: installing the
# app does not pull its debug extension.
# `flatpak remote-ls` prints full refs, `flatpak list` prints them without the
# kind prefix; both spellings are accepted here.
refs=$(flatpak remote-ls --user --all --columns=ref "$LOCAL_REMOTE" 2>/dev/null || true)

set --
if [ -n "$refs" ]; then
    while IFS= read -r ref; do
        case "$ref" in
            app/"$APP_ID"/*) set -- "$@" "$ref" ;;
            "$APP_ID"/*) set -- "$@" "app/$ref" ;;
            runtime/"$DEBUG_ID"/*)
                if [ "$WITH_DEBUG" = 1 ]; then set -- "$@" "$ref"; fi
                ;;
            "$DEBUG_ID"/*)
                if [ "$WITH_DEBUG" = 1 ]; then set -- "$@" "runtime/$ref"; fi
                ;;
        esac
    done <<EOF
$refs
EOF
else
    # The remote could not be listed: use the refs the export just wrote
    # (repo/refs/heads/<kind>/<id>/<arch>/<branch>).
    for ref in "$PWD"/repo/refs/heads/app/"$APP_ID"/*/*; do
        [ -f "$ref" ] || continue
        set -- "$@" "${ref#"$PWD"/repo/refs/heads/}"
    done
    if [ "$WITH_DEBUG" = 1 ]; then
        for ref in "$PWD"/repo/refs/heads/runtime/"$DEBUG_ID"/*/*; do
            [ -f "$ref" ] || continue
            set -- "$@" "${ref#"$PWD"/repo/refs/heads/}"
        done
    fi
fi
[ "$#" -gt 0 ] || die \
"$LOCAL_REMOTE ($repo_uri) does not provide $APP_ID
  check it with: flatpak remote-ls --user $LOCAL_REMOTE"

flatpak install --user --reinstall --noninteractive "$LOCAL_REMOTE" "$@"

# The debug runtime is opt-in: remove one left installed by an older build (or
# installed by hand) so it stops showing up as a pinned runtime in
# `flatpak uninstall --unused`.  Run the script with --debug to keep it.
if [ "$WITH_DEBUG" = 0 ]; then
    while IFS= read -r ref; do
        case "$ref" in
            runtime/"$DEBUG_ID"/*|"$DEBUG_ID"/*) ;;
            *) continue ;;
        esac
        say "removing the debug runtime $ref (use --debug to keep it)"
        flatpak uninstall --user -y --noninteractive "$ref" >/dev/null 2>&1 || true
    done <<EOF
$(flatpak list --user --all --columns=ref 2>/dev/null || true)
EOF
fi

# Local builds are updated by running this script, not by `flatpak update`.
flatpak remote-modify --user --disable "$LOCAL_REMOTE" ||
    warn "could not disable the $LOCAL_REMOTE remote"

# Now the old origin remotes have no installed refs left.  --force is what
# makes the deletion work: without it flatpak refuses (and prompts to remove
# the refs) when a remote still has installed refs.
for remote in $STALE_ORIGINS; do
    flatpak_has_remote --user "$remote" || continue
    if flatpak list --user --all --columns=origin 2>/dev/null | grep -qxF "$remote"; then
        warn "$remote is still the origin of an installed ref; keeping it"
        continue
    fi
    say "removing the old Flatpak remote $remote"
    flatpak remote-delete --user --force --noninteractive "$remote" >/dev/null 2>&1 ||
        warn "could not remove the Flatpak remote $remote"
done

say "installed $APP_ID (run it with: flatpak run $APP_ID)"
