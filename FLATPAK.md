# Building the Flatpak

`build-game-flatpak.sh` is the only supported way to build the game. It needs
`flatpak`, `flatpak-builder`, `python3`, `unzip` and `curl` (or `wget`), plus
your own copy of the Otomedius dump.

## Packages per distribution

| Distribution | Command |
|---|---|
| Fedora Workstation | `sudo dnf install flatpak flatpak-builder python3 unzip curl` |
| Fedora Silverblue / Kinoite | `flatpak` is preinstalled; `rpm-ostree install flatpak-builder && systemctl reboot` |
| Debian / Ubuntu / Mint / Pop!_OS | `sudo apt update && sudo apt install flatpak flatpak-builder python3 unzip curl` |
| Arch / Manjaro / EndeavourOS | `sudo pacman -S flatpak flatpak-builder python unzip curl` |
| openSUSE | `sudo zypper install flatpak flatpak-builder python3 unzip curl` |
| Alpine | `sudo apk add flatpak flatpak-builder python3 unzip curl` |
| Void | `sudo xbps-install -S flatpak flatpak-builder python3 unzip curl` |

If Flathub is not configured yet:

```sh
flatpak remote-add --if-not-exists --user flathub https://flathub.org/repo/flathub.flatpakrepo
```

The script installs the **Freedesktop Platform and SDK 26.08** automatically.

## Build

```sh
./build-game-flatpak.sh /path/to/otomedius.zip
```

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
cp "/path/to/your/otomedius.zip" game.zip
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
