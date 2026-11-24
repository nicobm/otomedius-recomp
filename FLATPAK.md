# Building the Flatpak

`build-game-flatpak.sh` is the only supported way to build the game. It needs
`flatpak`, `flatpak-builder`, `python3` and `curl` (or `wget`), plus your own
copy of the Otomedius dump. On Fedora Atomic the script uses Toolbx
automatically.

## Prerequisites

- A Flatpak host with Toolbx on Fedora Atomic (Silverblue, Bazzite, ...).
  `build-game-flatpak.sh` re-executes itself inside a toolbox container named
  `otomedius-recomp`, creating it if needed. The first create downloads the
  Fedora toolbox image automatically (`toolbox --assumeyes create`), and inside
  the container the script installs `flatpak` and `flatpak-builder` with `dnf`
  if they are missing. Use `OTOMEDIUS_TOOLBOX=name` to choose another
  container, or enter one yourself with `toolbox enter` and run the script
  there.

  On systems without Toolbx (Debian, Arch, Alpine, plain Fedora, ...) the
  script does not install anything on the host: it prints the package-manager
  command for the detected distribution (`apt`, `dnf`, `pacman`, `zypper`,
  `apk`, `rpm-ostree`, ...).

  To do it by hand instead:

  ```sh
  toolbox create --assumeyes --container otomedius-recomp
  toolbox enter --container otomedius-recomp
  # inside the toolbox, if needed:
  sudo dnf install flatpak flatpak-builder
  ```

- The Freedesktop runtime and SDK 26.08. `build-game-flatpak.sh` installs them
  automatically with `flatpak install -y` if they are missing. To install them
  manually instead:

  ```sh
  flatpak install -y flathub \
      org.freedesktop.Platform//26.08 org.freedesktop.Sdk//26.08
  ```

  If you prefer a per-user install, add Flathub to your user installation
  first and use `flatpak install -y --user flathub ...`.

- Your own game dump: a `.zip`, `.rar` or `.7z` archive, or the extracted
  folder with `otomedius.exe` and `Data/`. With no argument,
  `build-game-flatpak.sh` searches the XDG Downloads and Documents folders.
  Reading RAR/7z needs `bsdtar`, `unrar` or `7z`; ZIP needs nothing extra.
- `curl` or `wget` for the first build: the script downloads the capstone
  wheel into `flatpak/wheels/`. No source snapshot is needed.

## Packages per distribution (without Toolbx)

| Distribution | Command |
|---|---|
| Fedora Workstation | `sudo dnf install flatpak flatpak-builder python3 curl` |
| Debian / Ubuntu / Mint / Pop!_OS | `sudo apt update && sudo apt install flatpak flatpak-builder python3 curl` |
| Arch / Manjaro / EndeavourOS | `sudo pacman -S flatpak flatpak-builder python curl` |
| openSUSE | `sudo zypper install flatpak flatpak-builder python3 curl` |
| Alpine | `sudo apk add flatpak flatpak-builder python3 curl` |
| Void | `sudo xbps-install -S flatpak flatpak-builder python3 curl` |

## Build

From the repository root (the script enters the toolbox itself):

```sh
./build-game-flatpak.sh
```

With no argument the script searches the XDG Downloads and Documents folders
for a `.zip`, `.rar` or `.7z`, looks inside for `otomedius.exe`, checks its
SHA-256 against [`recomp/exe.sha256`](recomp/exe.sha256) and writes the
normalized `game.zip` the manifest expects. You can also pass the dump
explicitly. Spaces are fine without quotes (the script joins the words again),
but quote or escape shell metacharacters such as the parentheses and brackets
in dump names:

```sh
./build-game-flatpak.sh "/path/to/your/otomedius.zip"
./build-game-flatpak.sh /path/to/Otomedius.rar
./build-game-flatpak.sh "Otomedius (2008-04-18)(JPN)[Konami PC][TP].zip"
```

An existing `game.zip` is reused while its executable hash matches, so the
dump is not copied again on every build. Pass another dump or delete
`game.zip` to make the script search from scratch.

A cold build takes a few minutes on a modern machine (the recompiler produces
roughly 45 MB of C, which the SDK compiles with `-O2`).

The result is exported to the persistent `repo/` directory (ignored by git) and
installed for the current user through a single local remote called
`otomedius-local`. The remote is left **disabled**: the app is built locally,
so `flatpak update` has nothing to fetch and stays quiet, even if you later
move or delete the checkout. **To update the app, run
`./build-game-flatpak.sh` again**; it rebuilds and reinstalls from the local
repository.

`flatpak-builder` splits the debug symbols of the binary into a separate
`io.github.nicobm.OtomediusRecomp.Debug` runtime. It is **not** needed to play:
the script does not install it and removes one left by an older build, so it
does not appear as a pinned runtime in `flatpak uninstall --unused`. If you
want the symbols for `gdb`, run `./build-game-flatpak.sh --debug`.

If you prefer to run `flatpak-builder` by hand, make sure `flatpak/wheels/`
contains the capstone wheel first (run `./build-game-flatpak.sh` once, or
download it from the URL in the script), then:

```sh
python3 tools/find_game.py "/path/to/your/otomedius.zip"
flatpak-builder --user --force-clean --disable-rofiles-fuse --repo=repo \
    build-dir io.github.nicobm.OtomediusRecomp.json
flatpak remote-add --user --if-not-exists --no-gpg-verify --no-use-for-deps \
    otomedius-local "file://$PWD/repo"
flatpak install --user --reinstall otomedius-local \
    io.github.nicobm.OtomediusRecomp//master
flatpak remote-modify --user --disable otomedius-local
```

## Troubleshooting

* `flatpak update` reports `No se encontró la ref.
  «app/io.github.nicobm.OtomediusRecomp/x86_64/master» en el remoto
  otomediusrecomp-origin` (or `debug-origin`), or `Unable to update ...:
  Remote "..." not found`: this comes from an install made by an older
  `build-game-flatpak.sh`, which used `flatpak-builder --install`. That command
  creates throw-away `*-origin` remotes pointing at the repository of one
  particular build, so they break as soon as the checkout moves or the
  repository is rebuilt elsewhere. Run `./build-game-flatpak.sh` again: it
  reinstalls the app from `otomedius-local` and only then deletes the old
  `*-origin` remotes (in that order, so nothing is left pointing at a deleted
  remote). Deleting the old remotes by hand without reinstalling first makes
  `flatpak update` fail with `Remote ... not found` for the installed refs.

  If you only want to clean up without rebuilding, reinstall from the existing
  `repo/` first, then remove the remotes:

  ```sh
  flatpak remote-add --user --if-not-exists --no-gpg-verify \
      --no-use-for-deps otomedius-local "file://$PWD/repo"
  flatpak install --user --reinstall otomedius-local \
      io.github.nicobm.OtomediusRecomp//master
  flatpak remote-modify --user --disable otomedius-local
  flatpak remote-delete --user --force otomediusrecomp-origin
  flatpak remote-delete --user --force debug-origin
  ```

  `flatpak remotes` hides disabled remotes, so `otomedius-local` only shows up
  there with `flatpak remotes --show-disabled`.

## What the sandbox can and cannot do

The manifest uses only these permissions:

```json
"finish-args": [
    "--socket=wayland",
    "--socket=fallback-x11",
    "--socket=pulseaudio",
    "--device=dri",
    "--device=input",
    "--share=ipc",
    "--nofilesystem=home",
    "--nofilesystem=host",
    "--unshare=network"
]
```

* **Display**: native Wayland, with XWayland as a fallback.
* **GPU**: the DRM device only.
* **Input**: `/dev/input` (gamepads, keyboards).
* **Audio**: the PulseAudio socket.
* **No access** to your home directory, to the host filesystem or to the
  network.

On first start the launcher copies the game data into the application's own
writable directory
(`~/.var/app/io.github.nicobm.OtomediusRecomp/data/otomedius`) because the
game writes its bookkeeping files next to the data. That directory is the only
thing the application can write.

## The build itself has no network

`flatpak-builder` runs the build in a sandbox without network access. The only
downloaded items are:

* SDL3 3.4.20, pinned by SHA-256 in the manifest;
* the `capstone` wheel, downloaded by `build-game-flatpak.sh` into
  `flatpak/wheels/` (ignored by git).

## Uninstall

```sh
flatpak uninstall io.github.nicobm.OtomediusRecomp
flatpak remote-delete --user --force otomedius-local
rm -rf ~/.var/app/io.github.nicobm.OtomediusRecomp
rm -rf repo/
```

The `otomedius-local` remote and the `repo/` directory are only used by
`./build-game-flatpak.sh`; the remote is disabled, so `flatpak update` does not
need them.
