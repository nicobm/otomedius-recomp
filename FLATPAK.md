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
rm -rf ~/.var/app/io.github.nicobm.OtomediusRecomp
```
