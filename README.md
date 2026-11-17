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

## Working on this repo (session protocol)

This repository uses an explicit session protocol so a new AI session knows
what to do without guessing (full rules in [`AGENTS.md`](AGENTS.md)):

- **`INICIAR`** + GitHub account/repo/email/token: read `AGENTS.md`, the latest
  section of [`docs/BOOKLET.md`](docs/BOOKLET.md) and
  [`docs/HANDOVER_PROMPT.md`](docs/HANDOVER_PROMPT.md), check `git status` and
  run the smoke test before changing code.
- **`TERMINAR`**: run the tests, update `README.md`, the booklet and the next
  session prompt, then commit and push. **No commit/push without this signal.**
- The GitHub token is never written to a file; the push uses `GIT_ASKPASS` +
  an environment variable.

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
| `d3d9`/`d3dx9` objects, math, DDS textures | interfaces verified against `d3d9.h`; textures decode by pixel format (DXT1/3/5, RGB24, R5G6B5, A1R5G5B5, A4R4G4B4) and upload works; `ID3DXEffect` is a working stub (packages that compile HLSL effects now finish loading) |
| `dsound` (device, buffers, lock/play) | working; voices are mixed to SDL3 (volume/pan/rate/loop) |
| `dshow` (opening movie: graph, filter, pin) | interfaces working; the WMV3 intro is decoded with the runtime's ffmpeg and uploaded into the game's own video texture |
| Fonts / text | working: `GGO_GRAY8_BITMAP`/`GGO_BITMAP` implemented; `AddFontMemResourceEx` registers the game's own fonts (DFGothic/DFMincho/DFPOP…) and `SysFont.dds`/`gradius_font.dds` load through a real `D3DXCreateTextureFromFileExA` |
| Renderer (2D, sprites, playfield) | 2D path working (signed-area fix in `draw_tri`); the game runs its main loop, creates a 1280×1024 device and draws every frame (`BeginScene`/`EndScene`/`Present`); the attract HUD is visible; the 3D XYZ pipeline (`W*V*P`, `/w`, viewport, z-buffer, perspective-correct UVs, `DrawPrimitive`/`DrawIndexedPrimitive`) works and a HLE frame hierarchy draws a synthetic player ship; the real `.x` mesh parser and the shader camera are still pending |
| Audio | `dsound` voices are mixed to SDL3 (volume/pan/rate/loop); the intro movie track (WMAv2) plays cleanly through the same stream |

Today the recompiled game boots completely: the MSVC CRT runs, the window is
created, a Direct3D 9 device is created with valid caps at **1280×1024**,
DirectSound 8 comes up, the opening movie's filter graph is driven, the Konami
I/O board answers its boot handshake and the game enters its main loop.  The
x87 emulation was corrected (`fxch`/pop arithmetic), the software rasteriser
draws the 2D console overlay, and the real font atlas is generated with
FreeType and uploaded.  When the dump ships the cabinet art (`bezel.png`), the
game opens fullscreen with the arcade bezel on the sides and the frame fitted
into its transparent screen hole (`F11` goes to the configured window,
1366×768 by default; `OTOMEDIUS_WINDOW_SIZE=800x450` for a small 16:9 test
window).  The
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

The Konami logo is now visible: the loader no longer races (a thread
completion flag was uninitialised, which started two loader workers at once),
the attract loader gets through `Player02.pak` up to `Stage01.pak` without an
exception, and the native-thunk table is a fixed array (a `realloc` raced with
the audio mixer thread and crashed the attract).  The software rasteriser now
honours the real FVF, render states (alpha blend/test), stage-0 texture-stage
operations and the sampler's address/filter modes.

The `Title` background is now continuous: `main_visual.dds` is 24 bpp RGB
(not 32 bpp ARGB), so `decode_dds` now classifies by pixel format and
`texture_argb` understands `R8G8B8`, `X8R8G8B8`, `R5G6B5`, `A1R5G5B5` and
`X1R5G5B5`.  The same fix makes the attract tutorial (`catch_demo02`, also
RGB24) visible.  The `dsound` engine is no longer silent: each voice buffer is
mixed into one 48 kHz stereo stream with volume/pan/rate/loop, and the intro
audio no longer stutters (the audio ffmpeg decodes ahead instead of running
with `-re`).

The attract now runs: once the eye-catch ends, `CDemoPlayMan` builds
`CGameMan` and enters state 5, and the 2D HUD (player portrait, bars, buttons)
is drawn. The blocker was not the fade: `D3DXCreateEffect` returned `E_FAIL`,
which aborted the build of `Stage01.pak` (so the package never became ready and
`0x403DB0` suspended every manager with `mask220 = 0xFFFFFFFF`). A functional
`ID3DXEffect` stub lets the package finish loading. The port now also
registers the game's own font files through a real `AddFontMemResourceEx`,
keeps the file-mapping arena out of the heap (it used to overwrite live job
lists), shares one COM vtable per class (a per-object vtable exhausted the
fixed native-thunk table after a few thousand frames), and implements
`GetBackBuffer`/`GetDesc`.

What is still missing: the **real 3D layer**. The XYZ vertex path (FVF
`0x142`/`0x144`, `W*V*P`, z-buffer, viewport and perspective-correct UVs) now
works and is verified with a rotating cube, and a HLE frame hierarchy draws a
synthetic player ship while the `.x` parser is pending. The real `.x` mesh,
the shader camera/VIEW and the long warning text are still to do. The software
rasteriser runs at ~26–36 fps. The detailed state and the next three steps are
in [`docs/BOOKLET.md`](docs/BOOKLET.md) (section 12); the ready-to-paste
next-session prompt is [`docs/HANDOVER_PROMPT.md`](docs/HANDOVER_PROMPT.md).

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

## Testing (headless, silent)

[`tests/smoke.sh`](tests/smoke.sh) is the reproducible smoke test. It boots the
game with the software renderer and SDL's **dummy audio driver**, stops after a
fixed number of presents and checks: clean exit, no dispatch outside the
recompiled code, `CDemoPlayMan` reaching state 5, `CGameMan` leaving
`mask220 = 0`, and the native-thunk table not being exhausted.

```sh
# local agent tree (.work next to the repository)
OTOMEDIUS_SMOKE_LOG=/tmp/opencode/smoke.log sh tests/smoke.sh

# against another build/dump
OTOMEDIUS_BIN=out/otomedius-recomp OTOMEDIUS_DIR=/path/to/dump sh tests/smoke.sh
```

The script always sets `SDL_AUDIODRIVER=dummy`, so tests are quiet (including
the intro movie's audio). For manual runs use the same variable.

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
| `OTOMEDIUS_DUMP_FB=frame.ppm` | write one software frame (headless verification); Esc/window close also writes `frame.ppm.quit` |
| `OTOMEDIUS_DUMP_AT=n` | which frame to dump (default 200) |
| `OTOMEDIUS_TEST_QUAD=1\|2` | renderer self-test / texture preview |
| `OTOMEDIUS_TEXTURES=1` | real `LockRect`/surface uploads (see HANDOVER §7.6) |
| `OTOMEDIUS_CHKSUM=1` | keep the original `ChkSum.bin` integrity gate |
| `OTOMEDIUS_AUTOCOIN=1` | pulse the coin line every 2 s (input testing) |
| `OTOMEDIUS_BADTARGET=skip` | report missing functions instead of aborting |
| `OTOMEDIUS_BEZEL=path` | cabinet artwork (default: `bezel.png` next to the game) |
| `OTOMEDIUS_BEZEL=0` | disable the cabinet artwork |
| `OTOMEDIUS_WINDOWED=1` | start in a window instead of fullscreen |
| `OTOMEDIUS_WINDOW_SIZE=WxH` | windowed size (default 1366x768; tests use e.g. `800x450`) |
| `OTOMEDIUS_FULLSCREEN=1` | force fullscreen when there is no bezel |
| `OTOMEDIUS_MOVIE=1` | create the game's movie-player manager and show the intro video |
| `OTOMEDIUS_MOVIE_PREVIEW=1` | play the intro video on top of the frame, without the game's manager |
| `OTOMEDIUS_MOVIE_FILE=path` | override the movie file (host path) |
| `OTOMEDIUS_MOVIE_SIZE=WxH` | override the movie size used by the decoder (default 640x512) |
| `OTOMEDIUS_NATIVE_MOVIE=1` | leave the movie to the game's own `0x43F540` draw instead of compositing the texture (diagnostic) |
| `OTOMEDIUS_LOADER_GUARD=1` | force the async loader to report "done" after ~7 s (old workaround, off by default) |
| `OTOMEDIUS_BOOTDBG=1` | trace the boot/attract state machine, the loader (`LOADER` lines), the `CGameMan` constructor, `0x403DB0` log ids and corrupted job lists |
| `OTOMEDIUS_MALLOCDBG=1` | with `OTOMEDIUS_BOOTDBG`, also print the first 400 `MallocLocal` calls |
| `OTOMEDIUS_EFFECTDBG=1` | print the first `D3DXCreateEffect` shader blobs (size and ASCII preview) |
| `OTOMEDIUS_TRIPWIRE=addr[,addr…]` | abort with a dispatch trace when one of those dwords stops looking like code (wild-write detector) |
| `OTOMEDIUS_TEXDBG=1` | print every texture object creation and upload |
| `OTOMEDIUS_TEXDUMP=WxH` | dump the first decoded texture of that size to `/tmp/opencode/tex_<obj>.ppm` |
| `OTOMEDIUS_SYSFONT=1` | ignore the game's embedded fonts and use the system font |
| `OTOMEDIUS_WATCHDOG=1` | dump every emulated thread's state every 2 s (deadlock diagnosis) |
| `OTOMEDIUS_DUMP_EVERY=n` | with `OTOMEDIUS_DUMP_FB`, dump a frame every `n` presents starting at `OTOMEDIUS_DUMP_AT` (files get a `.NNNNN` suffix) |
| `OTOMEDIUS_DRAWDBG=n` | print every 2D draw (FVF, texture, render/sampler state, first vertices) of present `n`; also prints `SKIP`/`DISCARD` draws and the WORLD/VIEW/PROJ matrices |
| `OTOMEDIUS_TEXFMT=1` | print the DDS format/size of textures created from memory |
| `OTOMEDIUS_DSDBG=1` | log DirectSound buffer creation, Play/Stop, volume, frequency and Lock calls |
| `OTOMEDIUS_AUDIO_DUMP=file.raw` | dump the mixed dsound output (S16LE 48 kHz stereo) for analysis |
| `OTOMEDIUS_AUDIO_STREAM_DUMP=file.raw` | dump the final SDL audio stream (movie + dsound) |

## License

MIT for the code in this repository (see [`LICENSE`](LICENSE), © 2026 nicobm).
The game itself is not covered and not distributed.

## How this port is being written

The static recompilation, the reverse engineering and the native Linux layer in
this repository are being developed with several LLMs, driven through
**OpenCode** as the coding agent.

Everything runs against a local dump of the arcade build: the agent reads the
recompiled C, instruments the emulated Win32/D3D9 layer, and iterates on
`zig`/`flatpak-builder` builds from the command line. If you want to attempt the
same kind of port (Konami PC arcade titles share the `libacio`/`libbms`/
`libtouchpanel` stack, Direct3D 9 and a statically linked MSVC 2005 runtime),
that combination is a practical starting point - the tooling and the notes in
[`docs/BOOKLET.md`](docs/BOOKLET.md) are what made it tractable here.
