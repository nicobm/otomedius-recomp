# Otomedius — native Linux recompilation

A native **Linux x86-64** port of the 2008 Konami arcade shooter *Otomedius*,
built by **static recompilation** (AOT translation, not emulation): the
original 32-bit Windows executable is translated **ahead of time** into C on
your machine and linked against a hand-written Linux system layer (SDL3,
Vulkan, pthreads). No Wine, no emulator, no JIT, no interpreter, no
TeknoParrot.

What "static" means here, precisely:

* Every instruction of the game is decoded once, at build time, and becomes C
  code. At run time there is **no instruction decoder and no interpreter
  loop**: the CPU executes native x86-64 code produced by your C compiler.
* There is no emulated guest CPU state machine stepping through opcodes: the
  game's registers live in a plain C structure, its branches are C `goto`s and
  its calls are native calls plus a small dispatcher for computed targets.
* The only "emulated" thing that survives is the **32-bit address space**:
  the game keeps its original pointers, so its memory is mapped inside a
  reserved 4 GiB window of the 64-bit process.
* The result is a normal 64-bit Linux ELF binary: no 32-bit multilib, no
  i386 SDK, no compatibility layer.

> **Legal notice.** This repository contains **no part of the game**: no
> executable, no `.pak`/audio/video data, no DLLs, no code generated from the
> game. You need your own copy of the arcade dump. The build translates *your*
> executable on *your* machine and the result stays on your machine.
> Otomedius is © Konami Digital Entertainment. This project is not affiliated
> with, endorsed by or supported by Konami. Do not open issues asking where to
> find the game.

Version: see [`VERSION`](VERSION).

## Status

**Work in progress — not playable yet.** What exists today:

| Component | State |
|---|---|
| PE loader + 4 GiB emulated address space | working |
| x86 → C recompiler (`tools/recomp`) | working: ~5 000 functions, ~380 000 instructions, **95 %** of the code section translated |
| Native runtime (CPU state, dispatcher, x87, SSE2, string ops) | working |
| `kernel32` (files, heap, threads, TLS, code pages, time, file mappings) | working |
| Konami AVS layer (`libacio`, `libbms`, `libtouchpanel`) | offline/free-play cabinet emulation |
| `user32` window + message loop | minimal |
| `d3d9`/`d3dx9` objects, math, DDS textures | interfaces verified against `d3d9.h`; texture `LockRect`/upload works by default |
| `dsound` (device, buffers, lock/play) | interfaces working, silent |
| `dshow` (opening movie: graph, filter, pin) | interfaces working; the WMV3 intro is decoded with the runtime's ffmpeg and uploaded into the game's own video texture |
| Fonts / text | working: `GGO_GRAY8_BITMAP`/`GGO_BITMAP` implemented; the game's `SysFont.dds`/`gradius_font.dds` atlases are loaded through a real `D3DXCreateTextureFromFileExA` and text is legible |
| Renderer (2D, sprites, playfield) | 2D path working (signed-area fix in `draw_tri`); the game runs its main loop, creates a 1280×1024 device and draws every frame (`BeginScene`/`EndScene`/`Present`), including the console overlay; the 3D playfield path is still pending |
| Audio | `dsound` still silent; the intro movie track (WMAv2) plays through SDL3 |

Today the recompiled game boots completely: the MSVC CRT runs, the window is
created, a Direct3D 9 device is created with valid caps at **1280×1024**,
DirectSound 8 comes up, the opening movie's filter graph is driven, the Konami
I/O board answers its boot handshake and the game enters its main loop.  The
x87 emulation was corrected (`fxch`/pop arithmetic), the software rasteriser
draws the 2D console overlay, and the real font atlas is generated with
FreeType and uploaded.  When the dump ships the cabinet art (`bezel.png`), the
game opens fullscreen with the arcade bezel on the sides and the frame fitted
into its transparent screen hole (`F11` goes to a 1366×768 window).  The
opening WMV3 movie is decoded by the runtime's `ffmpeg` (the `codecs-extra`
runtime extension supplies the WMV3 decoder) and uploaded into the video
texture the game's own DirectShow renderer creates, with its WMAv2 audio
played through SDL3, so the intro can be shown (video + sound) with
`OTOMEDIUS_MOVIE=1` even while the boot sequence is still stuck.
`Esc` or the window button quits cleanly, also in the middle of the movie.

The boot/attract state machine now reaches every pre-game scene by itself
(`KonamiLogo` → `eAMUSELogo` → `Warning` → intro movie → `Title` → attract),
the intro movie advances on wall-clock time (~60 s), and the text uses the
game's real font atlases (`SysFont.dds`, `gradius_font.dds`) — the 180°
rotation that affected every textured quad was a rasteriser bug
(`fabs(area)` in `draw_tri`), now fixed.

What is still missing: the Konami logo texture does not survive the concurrent
`KonamiLogoSnd.pak` load (package id 9), `Player02.pak` raises an unhandled SEH
in the attract loader, the native movie quad still doubles its X scale (the
port composites the video instead), and the game's `dsound` audio is silent.
The detailed state and the next three steps are in
[`docs/HANDOVER.md`](docs/HANDOVER.md).

## Requirements

- Linux with Flatpak (Wayland recommended; X11/XWayland works through
  `fallback-x11`).
- Your own copy of the Otomedius arcade dump (the `.zip` used with
  TeknoParrot). The build checks its SHA-256 and the executable hash.

Nothing else: the SDK, SDL3 3.4.20 and the Python build dependency are fetched
by the build script.

## Build

```sh
git clone https://github.com/nicobm/otomedius-recomp
cd otomedius-recomp
./build-game-flatpak.sh /path/to/your/otomedius.zip
```

The script:

1. detects missing `flatpak` / `flatpak-builder` and prints the install command
   for your distribution (Fedora, Debian/Ubuntu, Arch, openSUSE, …);
2. verifies the SHA-256 of the archive and of `otomedius.exe`;
3. installs the Freedesktop runtime **and SDK 26.08** if they are missing;
4. downloads the pinned `capstone` wheel (the Flatpak build itself has **no
   network access**);
5. recompiles the game, builds the native layer, and installs the Flatpak for
   the current user.

Then launch **Otomedius (recompiled)** from your application menu or with:

```sh
flatpak run io.github.nicobm.OtomediusRecomp
```

See [`FLATPAK.md`](FLATPAK.md) for the package names per distribution and for
what the sandbox is allowed to access.

## Controls

| Action | Keyboard | Gamepad |
|---|---|---|
| Move | Arrows or W A S D | D-pad / left stick |
| Shot | Z or J | A (south) |
| Power-up / bomb | X or K | B (east) |
| Option / rapid | C or L | X (west) |
| Special | V or I | Y (north) |
| Start | Enter | Start |
| Coin | 5 or 6 | Back |
| Fullscreen | F11 | — |
| Quit | Esc | — |

Letter keys are read by physical position, so they stay in the same place on
AZERTY, QWERTZ and other layouts.

## How it works

```
 otomedius.exe ──tools/recomp/recomp.py──▶ recomp/gen/rc_*.c   (one C function
       │                                        │                 per x86 function)
       │                                        │
       └── native/ (kernel32, user32, d3d9, d3dx9, AVS) ──cc──▶ out/otomedius-recomp
                SDL3 + Vulkan (Flatpak runtime 26.08)
```

- The original image is mapped at its original base address inside a reserved
  4 GiB region, so every 32-bit pointer the game stored in its own memory keeps
  working.
- Every discovered x86 function becomes a C function. Registers and flags live
  in a per-thread `cpu_t`; branches inside a function are C `goto`s.
- `call`, `ret`, indirect jumps and tail jumps leave the generated function and
  come back through a dispatcher that maps a 32-bit address to a
  (function, continuation) pair, so return addresses are first-class entry
  points.
- `fs:` is intercepted for the Windows TEB/SEH chain; x87 uses `long double`
  (80-bit) and the SSE2 registers are modelled per thread.
- Imports are bound to native thunks: `call dword ptr [IAT]` becomes a direct
  call into the Linux implementation, with the Win32 stdcall cleanup applied by
  the wrapper table.

## Layout

| Path | Content |
|---|---|
| `tools/recomp/` | PE reader, x86 discovery/decoder, C emitter, driver |
| `native/` | native Linux system layer (runtime, Win32, Konami AVS, D3D9) |
| `recomp/exe.sha256` | hash of the executable the port is validated against |
| `flatpak/`, `*.json` | Flatpak packaging (manifest, icon, launcher) |
| `tests/` | headless platform backend used while debugging |

## Environment variables

| Variable | Effect |
|---|---|
| `OTOMEDIUS_LOG=1` | loader/import tracing |
| `OTOMEDIUS_LOG=2` | also log every Win32 call |
| `OTOMEDIUS_LOG=3` | also log every dispatcher transfer |
| `OTOMEDIUS_DIR=path` | game directory (default: the current directory) |
| `OTOMEDIUS_WATCH="addr[:n],…"` | trace entries to code addresses (hex) |
| `OTOMEDIUS_WATCHMEM="addr,…"` | dump those globals on every watch hit |
| `OTOMEDIUS_PEEKARGS=1` | also dump the stack arguments at watch hits |
| `OTOMEDIUS_DUMP_FB=frame.ppm` | write one software frame (headless verification) |
| `OTOMEDIUS_DUMP_AT=n` | which frame to dump (default 200) |
| `OTOMEDIUS_TEST_QUAD=1\|2` | renderer self-test / texture preview |
| `OTOMEDIUS_TEXTURES=1` | real `LockRect`/surface uploads (see HANDOVER §7.6) |
| `OTOMEDIUS_CHKSUM=1` | keep the original `ChkSum.bin` integrity gate |
| `OTOMEDIUS_AUTOCOIN=1` | pulse the coin line every 2 s (input testing) |
| `OTOMEDIUS_BADTARGET=skip` | report missing functions instead of aborting |
| `OTOMEDIUS_BEZEL=path` | cabinet artwork (default: `bezel.png` next to the game) |
| `OTOMEDIUS_BEZEL=0` | disable the cabinet artwork |
| `OTOMEDIUS_WINDOWED=1` | start in a window instead of fullscreen |
| `OTOMEDIUS_FULLSCREEN=1` | force fullscreen when there is no bezel |
| `OTOMEDIUS_MOVIE=1` | create the game's movie-player manager and show the intro video |
| `OTOMEDIUS_MOVIE_PREVIEW=1` | play the intro video on top of the frame, without the game's manager |
| `OTOMEDIUS_MOVIE_FILE=path` | override the movie file (host path) |
| `OTOMEDIUS_MOVIE_SIZE=WxH` | override the movie size used by the decoder (default 640x512) |
| `OTOMEDIUS_NATIVE_MOVIE=1` | leave the movie to the game's own `0x43F540` draw instead of compositing the texture (diagnostic) |

## License

MIT for the code in this repository (see [`LICENSE`](LICENSE), © 2026 nicobm).
The game itself is not covered and not distributed.

## How this port is being written

The static recompilation, the reverse engineering and the native Linux layer in
this repository are being developed with **DeepSeek V4.1 Flash 1017**, served by
**OpenRouter** and driven through **OpenCode** as the coding agent.

Everything runs against a local dump of the arcade build: the agent reads the
recompiled C, instruments the emulated Win32/D3D9 layer, and iterates on
`zig`/`flatpak-builder` builds from the command line. If you want to attempt the
same kind of port (Konami PC arcade titles share the `libacio`/`libbms`/
`libtouchpanel` stack, Direct3D 9 and a statically linked MSVC 2005 runtime),
that combination is a practical starting point - the tooling and the notes in
[`docs/HANDOVER.md`](docs/HANDOVER.md) are what made it tractable here.
