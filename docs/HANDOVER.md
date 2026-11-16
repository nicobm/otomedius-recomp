# Otomedius (Arcade) — Technical Handover

> Static recompilation of the 2008 Konami arcade shooter **Otomedius** to native
> Linux x86-64, packaged as a Flatpak. **This repository contains no game data**;
> the original executable is translated on the user's machine from their own
> dump. Repository: `https://github.com/nicobm/otomedius-recomp` (branch
> `master`).
>
> Estado: **el juego arranca de punta a punta** (loader, boot machine de Konami,
> NVRAM, instanciación de escena), el **x87 está corregido**, los **atlas de
> fuente se construyen y se suben** y el port es **estable 30 s sin crashes**.
> **Causa raíz del ruido de texto identificada**: el juego pide
> `GetGlyphOutlineA` con `fmt=6` = **`GGO_GRAY8_BITMAP`**, no `GGO_NATIVE` (2);
> nuestro `font.c` devuelve un *outline* y el rasterizador del juego lo lee como
> bitmap de 8 bpp. Ver §5.2. Además, el **boot de Konami ya corre** (los
> managers se actualizan y el quad del logo se dibuja), pero la textura real de
> `KonamiLogo.pak` aún no se carga porque el state machine del boot/attract
> manager se queda en el sub-estado 8. Ver §5.4.
>
> **Sesión 2026-11-16 (última)**: se añadió el **bezel de la cabina** (arte
> 1920x1080 con hueco de pantalla transparente, fullscreen por defecto y `F11`
> a 1366x768), se consiguió **decodificar y mostrar el vídeo de introducción**
> (WMV3 640x512, `Data/OTOMEDIUS_OP_f.wmv`) dentro de la **textura real del
> `TextureRenderer` del juego**, con **audio** (WMAv2 -> SDL3), y se corrigió
> el **cierre con Esc a mitad de vídeo** (cuelgue del hilo de audio + `call`
> a NULL en `0x4050B0`). Detalle completo en **§7**. Este documento es el
> prompt de transferencia para una sesión nueva; el repo es
> `/var/home/vm/Proyectos/SALA DE TRABAJO/otomedius-recomp`.
>
> **Última sesión: ver §9** (loader sin carreras, logo de Konami visible,
> tabla de thunks fija, rasterizador con estados D3D, fondo del Title roto).
> Donde §3–§8 contradigan a §9, manda §9.
>
> **Sesión más reciente: ver §10** (fondo del Title y attract resueltos:
> DDS RGB24/R5G6B5/A1R5G5B5, mezclador dsound→SDL, ventana configurable y
> dump al pulsar Esc). Donde §1–§9 contradigan a §10, manda §10.

---

## 1. Entorno y Toolchain

### 1.1 Target

| Item | Value |
|---|---|
| Host / target OS | Linux **x86-64 only** (no Windows, no macOS, no 32-bit build) |
| Delivery | Flatpak, runtime `org.freedesktop.Platform//26.08`, SDK `org.freedesktop.Sdk//26.08` |
| Display | Wayland native (`--socket=wayland`), XWayland fallback (`--socket=fallback-x11`) |
| Sandbox | `--socket=pulseaudio --device=dri --device=input --nofilesystem=home --nofilesystem=host --unshare=network` (**`--share=ipc` intentionally removed**) |
| UI / GPU | **SDL3 3.4.20** (built as a Flatpak module from upstream tarball, sha256 `f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f`), renderer in use: OpenGL (`platform: opengl renderer`) |
| Recompiler deps | Python 3 + **capstone 5.0.9** (wheel `capstone-5.0.9-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl`, sha256 `273fd8d747d2e35c88f91450be51a603ecfaafb00d96d9f315dcb8689c86193e`, fetched by the wrapper into `flatpak/wheels/`, gitignored) |
| C compiler in Flatpak | SDK `cc` with `-O2 -std=c11 -w -Inative $(pkg-config --cflags sdl3)` |
| Link line | `cc -O2 -o out/otomedius-recomp out/obj/*.o -lm -lpthread $(pkg-config --libs sdl3)` |

There is **no CMake and no MSVC**: the build is a single manifest script
(§1.3). Local agent builds use **zig 0.17.1** as `cc` (`.work/zig/zig`).

### 1.2 Local (agent) build, no Flatpak

* Compiler: `zig cc` (`<SALA DE TRABAJO>/.work/zig/zig`).
* Headless backend: `tests/platform_null.c` implements every `platform_*`
  symbol (no SDL, no GPU).
* Layout: `.work/gen` (generated C), `.work/obj` (native `.o`),
  `.work/obj/gen` (generated `.o`), `.work/oto` (extracted dump),
  `.work/pylib` (capstone + pefile), `.work/sdl3/include`.
* Helper scripts (**ahora en `.work/tools/`**, porque `/tmp/opencode` se vacía
  cuando OpenCode reinicia el catálogo de herramientas; `build.sh`, `xd.py`,
  `ppm2png.py`).  Antiguos, en `/tmp/opencode/`:
  `build.sh <src.c…>` (recompile natives + link), `gen_build.sh` (recompile the
  85 `rc_*.c` in parallel), `ppm2png.py`, `xdis_dump.py`, `hexdump.py`,
  `chk.py`, `xtr*.py` (binary xref scanners).  The local `build.sh` also adds
  `-I$W/freetype_include` and `-L$W/freetype_lib -lfreetype`; those headers
  were extracted from FreeType 2.13.2 into `.work/` because the agent container
  is the Platform runtime (no dev headers).  The Flatpak build uses the SDK's
  `pkg-config freetype2` instead.

```sh
# rebuild everything
sh /tmp/opencode/build.sh avs.c config.c patches.c …   # natives + link
sh /tmp/opencode/gen_build.sh                          # 85 generated files
# regenerate the C from the exe (seeds are read from recomp/seeds.txt)
PYTHONPATH=../.work/pylib python3 tools/recomp/recomp.py \
    --exe ../.work/oto/otomedius.exe --out ../.work/gen --chunk 60
# run headless
cd ../.work/oto && ../obj/otomedius-recomp .
```

> **CRITICAL BUILD RULE:** `cpu_t` lives in `native/x86rt.h` and is embedded in
> every generated file. **Any change to that header requires rebuilding ALL
> objects, including the ~85 generated `rc_*.c`.** Mixing stale objects produced
> silent corruption and wasted hours (boot-time SIGSEGV at a "heap" address).
> The Flatpak build always compiles from scratch, so it never had this problem.

### 1.3 Build pipeline

`build-game-flatpak.sh` (bash, distro-agnostic: prints `dnf`/`apt`/`pacman`/
`zypper`/`apk`/`xbps` hints when `flatpak`, `flatpak-builder`, `python3`,
`unzip`, `curl` are missing):

1. Locates the dump (arg, `game.zip`, or any `./*.zip`), verifies
   `sha256 == 4c6562eab3fff1bd1a6e84732bcb565a871440fcb894985fe990fe566a8bd897`
   (warning only), copies it to `game.zip`.
2. Installs runtime + SDK 26.08 (user remotes first, then system, then Flathub).
3. Downloads the capstone wheel (sha256-checked).
4. `flatpak-builder --user --install --force-clean --disable-rofiles-fuse --repo=repo build-dir io.github.nicobm.OtomediusRecomp.json`

Manifest module `otomedius-recomp`, `build-commands` (single script, no helpers):

```
python3 tools/recomp/recomp.py --exe data/otomedius.exe --out recomp/gen \
        --expect-sha256 "$(cat recomp/exe.sha256)"
find native -name '*.c' -print0 | xargs -0 -P "$(nproc)" -I{} sh -c \
  'f="$1"; cc -O2 -std=c11 -w -Inative $(pkg-config --cflags sdl3) \
     $(pkg-config --cflags freetype2) -c "$f" -o "out/obj/$(basename "$f" .c).o"' _ {}
find recomp/gen -name 'rc_*.c' -print0 | xargs -0 -P "$(nproc)" -I{} sh -c \
  'f="$1"; cc -O2 -std=c11 -w -Inative -c "$f" -o "out/obj/$(basename "$f" .c).o"' _ {}
cc -O2 -o out/otomedius-recomp out/obj/*.o -lm -lpthread \
   $(pkg-config --libs sdl3) $(pkg-config --libs freetype2)
```

Game data → `/app/share/otomedius-recomp/game/`; the launcher
`flatpak/otomedius-recomp-launch` copies it once to `$XDG_DATA_HOME/otomedius`
(writable) and **copies `otomedius.ini` to `oto.ini`** because the game opens
`oto.ini` (the dump ships `otomedius.ini`).

### 1.4 Directory structure (current)

```
otomedius-recomp/
├── README.md  FLATPAK.md  LICENSE (MIT © 2026 nicobm)  VERSION (0.1.0)
├── build-game-flatpak.sh                 # the only user-facing script
├── io.github.nicobm.OtomediusRecomp.json # Flatpak manifest (python-capstone, sdl3, otomedius-recomp)
├── flatpak/                              # launcher, .desktop, .svg, .metainfo.xml, wheels/
├── native/                               # the Linux system layer (18 files, ~7.5k lines)
│   ├── main.c            boot, fault handler, watchdog, x86_thread_begin, ExitThread longjmp
│   ├── memory.c          MEM mmap, PE image load, TEB/PEB setup
│   ├── imports.c         PE import binding, native thunks, x86_native_call/tail/invoke
│   ├── patches.c         NEW: address overrides (ChkSum bypass, loader guard, finalise skip)
│   ├── config.c          NEW: oto.ini loader (game's own variable table) + loader-ring init
│   ├── x86rt.h/.c        cpu_t, dispatcher, watch/watchmem/watchchg hooks, ALU/x87/SSE2
│   ├── win32.h           shared helpers (ARG32, RET32, log_printf…)
│   ├── win32_kernel32.c  files, heaps, vm, threads, TLS, code pages (iconv), time, mappings
│   ├── win32_user32.c    window + message queue
│   ├── win32_misc.c      gdi32 (incl. GetGlyphOutlineA), ole32/oleaut32/shlwapi/imm32, winmm
│   ├── d3d9.h/.c         IDirect3D9 (17 methods) + IDirect3DDevice9 (119), D3DCAPS9
│   ├── d3d9_objects.c    texture(22)/surface(17)/buffer(14)/shader(5) + LockRect + DXT decode
│   ├── d3dx9.c           maths, DDS header parse, D3DX stubs
│   ├── dsound.c          IDirectSound8(12) + IDirectSoundBuffer8(24) (silent)
│   ├── dshow.c           fake DirectShow graph (drives the game's own TextureRenderer filter)
│   ├── avs.c             Konami libacio/libbms/libtouchpanel replacement (+ touch input)
│   ├── render.c          software rasteriser, frame stats, PPM dump, renderer self-test
│   ├── font.c / font.h   NEW: FreeType GDI backend (metrics, ABC, GGO_NATIVE outlines)
│   └── platform.h/_sdl.c SDL3 window, events, input, framebuffer upload
├── tools/recomp/         pe.py (own PE parser), xdis.py (discovery), emit.py (x86→C), recomp.py
├── recomp/exe.sha256     c55796ad1f2d6ea9bd0fb0d33ed6b6eb6463d0e3ad8f2e1d7c4608ca9dfcf112
├── recomp/seeds.txt      NEW: 7 manual entry points the discovery misses
├── tests/platform_null.c headless backend
└── docs/HANDOVER.md      this file
```

### 1.5 Diagnostic switches (all implemented, keep them)

```sh
OTOMEDIUS_LOG=0|1|2|3            # 0 quiet, 1 boot/file/thread, 2 Win32+D3D detail, 3 per-dispatch
OTOMEDIUS_WATCH="addr[:n],…"     # print entries to code addresses (hex) + registers
OTOMEDIUS_WATCHMEM="addr,…"      # dump those dwords on every watch hit
OTOMEDIUS_PEEKREGS=1             # also dump [esi]/[ecx] object fields at a hit
OTOMEDIUS_PEEKARGS=1             # also dump the stack arguments at a hit
OTOMEDIUS_WATCHCHG=auto|<addr>   # poll a dword every dispatch; on change print old/new + who
OTOMEDIUS_VERTDBG=1              # dump the first draw of every 120th frame (vertices+UV)
OTOMEDIUS_DUMP_FB=/tmp/x.ppm     # write one frame (P6) — headless verification
OTOMEDIUS_DUMP_AT=n              # which frame to dump (default 200)
OTOMEDIUS_TEST_QUAD=1|2          # 1 = full-screen quad test, 2 = texture preview
OTOMEDIUS_TEST_TEX=0xADDR        # texture object to preview with TEST_QUAD=2
OTOMEDIUS_TEXTURES=1             # real LockRect + surface sharing (now the default)
OTOMEDIUS_TEXTURES=0             # restore the old "LockRect always fails" path
OTOMEDIUS_CHKSUM=1               # keep the original ChkSum.bin integrity gate
OTOMEDIUS_LOADER_GUARD=1         # OPT-IN: force the loader to finish (52 polls / 70 polls);
                                 # off by default since §9.1 (NO_LOADER_GUARD no longer exists)
OTOMEDIUS_FORCE_JOB=1            # experiment: relax the 0x419E30 gate (breaks the boot)
OTOMEDIUS_WATCHDOG=1             # every 2 s: per-thread eip/regs + current native import
OTOMEDIUS_AUTOCOIN=1             # pulse the coin line every 2 s (input testing)
OTOMEDIUS_AUTOSTART=1            # pulse start
OTOMEDIUS_BADTARGET=skip         # report missing functions instead of aborting
OTOMEDIUS_NOTHREDS=1             # create thread handles but never run them
OTOMEDIUS_NOMOVIE=1              # force VFW_E_NOT_FOUND
OTOMEDIUS_NO_FREETYPE=1          # force the synthetic rectangle glyph path
OTOMEDIUS_FONT=/path/font.ttf    # override the FreeType font (CJK, etc.)
OTOMEDIUS_TEXDBG=1               # dump each font atlas to /tmp/opencode/tex_*.pgm
OTOMEDIUS_KONAMI=1               # experimental: drive the KonamiLogo manager
                                 # directly from render.c (0x439920 + 0x4395B0)
OTOMEDIUS_DIR=path               # game directory
OTOMEDIUS_BEZEL=path|0           # cabinet artwork (default: <game>/bezel.png)
OTOMEDIUS_WINDOWED=1             # start windowed (1366x768) instead of fullscreen
OTOMEDIUS_FULLSCREEN=1           # force fullscreen (also without a bezel)
OTOMEDIUS_BEZEL_DUMP=file.ppm    # dump the composed window (bezel+game)
OTOMEDIUS_BEZEL_DUMP_AT=n        # present at which to dump (default 0 = first)
OTOMEDIUS_MOVIE=1                # create the movie manager and show the intro
OTOMEDIUS_MOVIE_PREVIEW=1        # show the intro over the frame (no manager)
OTOMEDIUS_MOVIE_FILE=path        # override the movie file (host path)
OTOMEDIUS_MOVIE_SIZE=WxH         # decoder size (default 640x512)
OTOMEDIUS_MOVIE_DBG=1            # movie/dshow diagnostics
OTOMEDIUS_NATIVE_MOVIE=1         # skip the port's video composition, draw the
                                 # movie with the game's own 0x43F540 quad
OTOMEDIUS_BOOTDBG=1              # boot/attract state machine + local heap trace
OTOMEDIUS_QUIT_AT=n              # request quit at present n (shutdown testing)
OTOMEDIUS_DUMP_EVERY=n           # with DUMP_FB: dump every n presents from DUMP_AT (file.NNNNN)
OTOMEDIUS_DRAWDBG=n              # print every 2D draw of present n (fvf, tex, rs/tss/ss, verts)
OTOMEDIUS_TEXFMT=1               # print the DDS format/size of D3DXCreateTextureFromFileInMemoryEx
```

---

## 2. Mapa de Binarios y Memoria

### 2.1 The dump

* `Otomedius (2008-04-18)(JPN)[Konami PC][TP].zip` — sha256
  `4c6562eab3fff1bd1a6e84732bcb565a871440fcb894985fe990fe566a8bd897` (~452 MB).
* Contents: `otomedius.exe`, `otomedius_edit.exe`, `bms_GGG.exe`, `d3dx9_32.dll`,
  `libacio.dll`, `libavs-win32.dll`, `libbms.dll`, `libtouchpanel.dll`,
  `dbghelp.dll`, `HIdGet.exe`, `HidBoot.id`, `ChkSum.bin`, `bookkeep.bin`,
  `ea_conf.bin`, `testmode.bin`, `card_allow.list`, `otomedius.ini`,
  `Data/*.pak` (151 files, 436 MB, Microsoft Cabinet `MSCF`), `Data/*.xwb`
  (xWMA), `Data/OTOMEDIUS_OP_f.wmv`, `Data/*.dds`.
* **This dump is a later revision than `ChkSum.bin` expects**: 169 of the 191
  listed files match, **22 do not** (first: `Data/CardEntry.pak`). See §4.6.

### 2.2 otomedius.exe

| | |
|---|---|
| Format | PE32, i386, 4 sections, GUI subsystem |
| Size | 1 945 600 bytes |
| SHA-256 | `c55796ad1f2d6ea9bd0fb0d33ed6b6eb6463d0e3ad8f2e1d7c4608ca9dfcf112` |
| Image base | `0x400000` |
| Entry (`AddressOfEntryPoint`) | `0x00530e2b` |
| Sections | `.text 0x1000 sz 0x14b6b7` · `.rdata 0x14d000 sz 0x452e0` · `.data 0x193000 sz 0x54fa8` · `.rsrc 0x1e8000` |
| PDB | `c:\Local\proj\otome\soft\src\prog\Release\moedius.pdb` |
| Compiler | **MSVC 2005 Release**, x87 (no SSE in game code), SEH via `fs:[0]` |
| Imports | 300 functions bound, 0 missing |

**`.data` is only partly file-backed.** Everything from roughly `0x5D0000` up
(`vsize` 0x54fa8 > raw size) loads as **zero (BSS-like)**: the async loader
ring, the local-heap descriptors, the package registry and the config globals
all live there. Three bugs this session were "uninitialised .data" (§4.4, §4.5,
§4.7). Verify with `pe.read(va-0x400000, n)` → `PEError: not backed by file data`.

Import DLLs: `d3d9.dll`(1), `d3dx9_32.dll`(34), `imm32.dll`(1), `winmm.dll`(5),
`libacio.dll`(32), `libtouchpanel.dll`(13), `libbms.dll`(66),
`dsound.dll`(ordinal **11** = `DirectSoundCreate8`), `kernel32.dll`(104),
`user32.dll`(27), `gdi32.dll`(7), `ole32.dll`(6), `oleaut32.dll`(ordinals
**2**=`SysAllocString`, **6**=`SysFreeString`), `shlwapi.dll`(1).

gdi32 imports (relevant to the active blocker): `CreateFontA`,
`DeleteObject`, `GetGlyphOutlineA`, `GetTextMetricsA`, `GetCharABCWidthsA`,
`AddFontMemResourceEx`, `SelectObject`.

### 2.3 Emulated 32-bit address space

| Range | Use |
|---|---|
| `0x00000000`–`0xFFFFFFFF` | one 4 GiB `mmap(MAP_NORESERVE)`; `MEM` points at it |
| `0x00400000`–`0x005E9000` | PE image at its original base |
| `0x000A0000`+ | CLI string, PEB fields |
| `0x02000000`+ | per-thread stacks (1 MiB each; slot N top = `0x02000000+(N+1)*1MiB`) |
| `0x0A000000`–`0x20000000` | `MapViewOfFile` arena (bump); `Once.pak` maps at `0x0A001000` |
| `0x10000000`–`0x30000000` | `HeapAlloc` arena (`hdr_t{size,used,heap,magic=0x4F544D48}`, first-fit) |
| `0x30000000`–`0x60000000` | `VirtualAlloc` arena (bump) |
| `0x60000000`+ | COM objects + `emu_alloc` bump (textures, buffers); reached `0x66bae320` |
| `0x7F000000` | native thunks, **16 bytes apart**, `idx = (addr-0x7F000000)/16` |
| `0x7FF00000`+ | per-thread TEB (0x2000 each) |
| `0x7FFDE000` | main TEB (SEH head `+0x00`, stack top `+0x04`, bottom `+0x08`, self `+0x18`, PEB `+0x1C`, pid `+0x24`, tid `+0x28`, TLS `+0x2C`, last error `+0x34`, TLS array `+0x1000`) |
| `0x7FFDF000` | PEB |

`X86_RET_SENTINEL = 0xFFFFFFF1`.

### 2.4 Game globals (all verified at runtime)

| Address | Meaning / evidence |
|---|---|
| `0x005E1F1C` | second `IDirect3D9*` (caps/device selection path `0x4012f4`) |
| `0x005E1F20` | **`IDirect3DDevice9*`** (read by `0x402490`, `0x4024a0`, `D3DXCreateTexture` at `0x409884`) |
| `0x005E1F24` | frame-update gate (`cmp byte [0x5e1f24], 1; je fail` at `0x402575`) |
| `0x005E1F26` | pacing flag after `timeGetTime` (`0x402682`) |
| `0x005E1F27` | **"rendering enabled"**, set by `WM_ACTIVATE` (`0x401BE1`) |
| `0x005E1F28` | **local heap descriptor** (`[+0]`=name ptr `0x55614C`, `[+4]`=first block `0x100015B8`) — used by `0x4142B0` |
| `0x005E1F50` | **HWND** |
| `0x005E1F58` / `0x005E1F59` | windowed / device-lost flags |
| `0x005E1F7C` | log object pointer (`0x10006278`), 255-byte buffer at `+0x51` |
| `0x005E1F82` | `== 1` skips the manager loop (`0x402563`) — never written by recompiled code |
| `0x005E1F88` / `0x005E1F8C` | `CSound*` (set by `0x404FB0`, read by `0x405120`) / secondary sound object |
| `0x005E1F96` | **boot state machine index 0..4** (jump table `0x40C9FC`) — measured **2** in the steady state |
| `0x005E1FA5` | I/O board mode, **5 = PJEI** (`0x42D407`) |
| `0x005E1FA6` | touch-panel mode, **3 = active** (`0x42D41A`) |
| `0x005E1FA7` | card-unit state, **> 1 required** (`0x42D427`); 2 = ready |
| `0x005E1FFD` / `0x005E1FFE` | I/O started / `ac_io_update` enabled |
| `0x005E2000` | **touch-panel handle** (`ac_touchpanel_new`) |
| `0x005E2026` | counter tested as a **byte** at `0x42D62A` (runtime value `0x200` → byte 0) |
| `0x005E2027` | **asset/scene request**: 0 = load, 1 = `Once.pak`, 2 = waiting, **0xFF = done** |
| `0x005BB434` | one-shot flag: `cmp [0x5bb434],1` → run the scene creation once (`0x42D63C`) |
| `0x005BC914` | handle of the loader's job thread (`0x1010`/`0x1014`) |
| `0x005BC918` | id of the job being processed |
| `0x005BC91C` | **loader state**: 0 idle, 1 running, 2 done, 3 work ready, 4 error |
| `0x005B3F82` | loader-thread flag: 0 on entry, 1 on exit (`0x419D16`/`0x419E0E`) |
| `0x005C324C`…`0x005C3258` | 4 arguments used by the finalise step `0x414920` |
| `0x005DCF8C` | ring "current id" |
| `0x005DCF90` / `0x005DCF94` | ring **head** / **tail** (equal ⇒ empty) |
| `0x005DCF98` | **async loader ring**: 16 slots × `0x10C`; `[+4]` = status, **`-1` = free** |
| `0x005DC9A2` | loader-active flag (`cmp byte [0x5dc9a2],0` at `0x419E30`, `0x419170`) |
| `0x005DC98B` | current switch state byte (bit0 tested by the scene manager `0x42D530`) |
| `0x005DC9BC` | merged 16-bit switch word |
| `0x005DCF70` | output array of the finalise step (`0x414941`) |
| `0x005DE050`… | **global singleton**: CRITICAL_SECTIONs at `0x5DE058` and `0x5DE070`, **package registry table at `0x5DE098`** (4 entries, `[i*4+0x5DE098]`) |
| `0x005DE128` / `0x005DE12C` | sound thread id / started flag |
| `0x005DE220` | **manager enable mask** (`0xFFFFFFFF` = suspend everything; tested by `0x439FE0`) |
| `0x005DE228` | manager slot array (0x5DE228…0x5DE27C); `[0]` = the scene manager object |
| `0x005DB130`–`0x005DB278` | config block, **zeroed by the game** with `memset(0x5DB130,0,0x148)` at `0x401EC6` |
| `0x005DB150` | `wSortDrawCmn2DMax` (u16, from `oto.ini`; **must be 256** or the first `CSortData` registration overflows → error 1115 → all managers suspended) |
| `0x005BE328` | **ini variable table**: `{const char *name; void *target; int type}` × 97, NULL-terminated; type 1 = byte, 3 = u16, 6 = u32, 7 = float |
| `0x005BE388` | `wSortDrawCmn2DMax` entry (`0x5507D0`, `0x5DB150`, 3) |
| `0x005E6E60` / `0x005E6E28` | SSE2/runtime dispatch flags |

### 2.5 Key constants

* Device caps checked at `0x4012D9`: `MaxStreams >= 3`, `MaxPrimitiveCount >= 0xFFFF`, `MaxVertexIndex >= 0xFFFF`.
* `D3DFMT_X8R8G8B8 = 0x16`; `D3DERR_DEVICELOST = 0x88760868`; `D3DERR_DEVICENOTRESET = 0x88760869`; `VFW_E_TYPE_NOT_ACCEPTED = 0x8004022A`; `VFW_E_NOT_FOUND = 0x80040216`; `E_NOINTERFACE = 0x80004002`.
* `HANDLE_BASE = 0x1000`, one handle every 4 bytes, `MAX_HANDLES = 4096`.
* `ChkSum.bin` = `u32 count (191)` then `{char name[]; u8 md5[16]}` × 191.

---

## 3. Funciones Recompiladas y Subsystems Resueltos

### 3.1 Recompiler (`tools/recomp`, own implementation, capstone-based)

* `recomp.py --exe X --out recomp/gen [--chunk 60] [--expect-sha256 H] [--seed ADDR] [--seeds-file recomp/seeds.txt]`
* Discovery order: entry+exports+data-pointer seeds → fixpoint over call targets →
  immediate code pointers → **prologue heuristic last** → fixpoint of branch
  targets / call-return sites → **adoption** of data words pointing to unowned
  code (only if not inside an already decoded instruction).
* Jump tables: `jmp dword ptr [reg*4+table]` read strictly from `table`.
* Output: **5 022 functions**, **381 616 instructions**, **95.7 %** of `.text`,
  **78 955 dispatch entries**, 85 files (`rc_000.c`…`rc_084.c`).
* Code model: one C function per x86 function, C `goto` for intra-function flow,
  `cpu->eip = target; return;` for inter-function transfers, dispatcher
  `x86_entries[]` (sorted, binary search) mapping address → (fn, continuation).
  `cpu->cont` selects the resume label; every return address is a table entry.
* **Only entry addresses (`cont == 0`) and call-return sites are dispatched**;
  intra-function jump targets are `goto` labels and are **not observable** with
  `OTOMEDIUS_WATCH`.
* Unhandled mnemonics (unreachable so far): `int3`(59), `in`(14), `out`(7),
  `les`(6), `aaa`(5), `hlt`(4), `jecxz`(2), `aam`(2), `lcall`(2),
  `cmpltpd`(2), `cmpeqsd`(2), `int`(1).
* `recomp/seeds.txt` holds the 7 entry points that only appear through computed
  pointers: `419700 40f4d0 40f570 40f5c0 40f5f0 419730 419740`. Without them the
  game dies with `no recompiled code at …` while loading `Once.pak`.
  A blanket "seed after every `int3` run" heuristic was tried (coverage 97.0 %)
  and **reverted**: it promotes already-misdecoded padding into live functions
  and produced an infinite `_chkstk` recursion.

### 3.2 Native runtime (`native/x86rt.*`)

`cpu_t`: 8 GPRs, eip, 6 flags, df, `cont`, `seg[6]`, `xmm[8][2]`, mxcsr, x87
`st[8]` as `long double` + tag/top/cw/sw/cc, `teb`, `gs_base`, `entry_esp`,
`esp_check`. `x86_cpu()` is `__thread`.
Helpers: ALU with full flag semantics, shifts/rotates, `mul/div/imul`, string
ops, `cpuid` (SSE2/SSE3, no SSE4/AVX), `rdtsc`, `fs:`/`gs:` → TEB, full x87,
SSE2 subset, `x86_call_stdcall` (native → recompiled).

Dispatcher extras added this session: `x86_watch_hit`, `x86_peek_dump`,
`x86_watchchg_poll`, `x86_dump_threads`, per-thread `x86_import_name[]`, and
`x86_patch_call()` (called **before** the native-thunk check).

### 3.3 Game functions identified

| Address | Role |
|---|---|
| `0x00530E2B` | `_WinMainCRTStartup` (calls `0x53B2AD`, then `jmp 0x530C4B`) |
| `0x0053B2AD` | `__tmainCRTStartup` (locale, heap, TLS, `ImmDisableIME`, calls WinMain) |
| `0x00530547` | `_threadstartex`-like CRT thread wrapper; real entry at `[param+0x54]`, arg at `[param+0x58]` |
| `0x0051BA88` | thunk → `imm32!ImmDisableIME` |
| `0x0051BA10` | thunk → `d3dx9_32!D3DXCreateTexture` |
| `0x0052DB7A` | thunk → `dsound!DirectSoundCreate8` |
| `0x00401BB0` | **window procedure**; msg jump table `0x401D74`, byte index table `0x401D94` (`msg-2`); `WM_ACTIVATE(6)` → `0x401BE1` (`setne cl; mov [0x5E1F27],cl`), `WM_DESTROY(2)` → `0x401BF2` |
| `0x00401100` | CRT `WinMain` caller path |
| `0x00401EC6` | `memset(0x5DB130, 0, 0x148)` — the game's own config reset (see `config.c`) |
| `0x00402230` | **shutdown path** (`0x439FB0`, `CSound::Move` `0x4050B0`, `0x418190`, sound service `0x41A9A0`) |
| `0x004024C0` | **frame update**: `TestCooperativeLevel` (vtbl 3), `0x401A00`, `0x412FE0`, `0x439FE0`, `0x41AA10`, then `0x402340`; returns `al` (0 ⇒ the loop destroys the window). **Stops being called once the game leaves the loading loop** |
| `0x00402340` | **render**: `0x40F780` (draw pass), `EndScene` (42), `Present` (17) |
| `0x004025C0` | **message loop**: `PeekMessageA`, `TranslateMessage`, `DispatchMessageA`, `0x4024C0`, `DestroyWindow`, `timeGetTime`; exits on `msg.message == 0x12` |
| `0x00402E90` / `0x00402760` | lazy-init wrappers → `0x402F10` (LoadConfig) / `0x4027A0` (LoadBookKeep); return `al` (0 = failure) |
| `0x00402F10` | **LoadConfig**: `0x40EC30("E:\nvmem\testmode.bin", …)`, checks size `0x48` and magic `[0x5DB708] == 0x07091300` |
| `0x004027A0` | **LoadBookKeep**: `0x40EC30("E:\nvmem\bookkeep.bin", …)`, size `0x290`, magic `[0x5DB478] == 0x20070404` |
| `0x0040EC30` / `0x0040EFB0` | generic "read whole file" (`CreateFileA`/`GetFileSize`/`malloc`/`ReadFile`) |
| `0x0040ED00` | **integrity check**: MD5 of one `ChkSum.bin` entry (patched, §4.6) |
| `0x00403DB0` | "log a message with a 4-digit id": formats into `[0x5E1F7C]+0x51`, parses 4 digits, `call 0x405120(id)`; **sets `[0x5DE220] = 0xFFFFFFFF`** (suspends every manager) |
| `0x00405120` | event/log sink (`0x405120(id, arg)`) — NOT `CSound::Init` |
| `0x00403070` | draw manager entry used by the frame |
| `0x0040C7F0` | **Konami boot state machine**: `Sleep(16)`, `ac_touchpanel_polling`, `ac_io_update`, then jump table `0x40C9FC = {0x40C85C, 0x40C8E1, 0x40C9C7, 0x40C9EB, 0x40C9F3}`; state 2 (`0x40C9C7`) calls `bms_is_active` and returns **2 = ready** |
| `0x0042D3D0` | **scene/asset manager** (thiscall, `this` in `ecx`, registered in slot array `0x5DE228`, updated by `0x439FE0`). Gates at entry: `[0x5E1FA5]==5`, `[0x5E1FA6]==3`, `[0x5E1FA7]>1`. Sub-state table `0x42DBA4 = {0x42D530(0), 0x42D558(1), 0x42D5A0(2), 0x42D89A(3), 0x42D8DA(4), 0x42D921(5), 0x42D96D(6), 0x42D9D3(7)}`. State 0 → sub-state 2; sub-state 2 calls `0x40C7F0` then, on `al==2`, the post-boot path (`0x402E90`, `0x402760`, `0x402FB0`, `0x402970`, `0x402840`) and finally the scene creation at `0x42D9F8` |
| `0x0042D9F8` | **scene creation**: `HeapAlloc(0x5E1F28, 0x3C)` → `0x45F1B0`, `HeapAlloc(0x5E1F28, 0x18)` → `0x439C80`, `HeapAlloc(0x5E1F28, 0x420)` → `0x44D190`, … (runs **once**, guarded by `[0x5BB434]`) |
| `0x0042DC30` | scene/message handler object used by the manager's loop (draws through `0x40F780`) |
| `0x0043A030` | **manager loop** (`0x439FE0`): for each slot, `[obj+4] & [0x5DE220]` → skip; else `call [vtbl+8]`; `al==0` ⇒ `call [vtbl]` and clear the slot |
| `0x00412EA0` | **I/O init**: `ac_io_begin` → window → `ac_touchpanel_new` → `ac_touchpanel_caribrate_set` → `[0x5E1FFD]=1` |
| `0x00412FE0` | **per-frame I/O update**: saves/merges the switch word, calls `0x4132C0` |
| `0x004132C0` | **cabinet detection state machine**: jump table `0x413BD0` (7 modes) + touch-panel machine (`0x413BEC`) + card-unit machine (`0x413C0C`) |
| `0x00413CD0` | per-frame misc (touch-panel poll, `ac_io_update`) — harmless, does **not** destroy windows |
| `0x004142B0` | **game local-heap allocator** (`HeapAlloc` on `0x5E1F28`-style descriptors) |
| `0x00414460` | game `free` |
| `0x00414920` | **finalise/commit step** (called when the load reports done): `edi = 0x419690(0)`, then 4× `call [ [edi] + 0x24 ]` with `[0x5C324C+i*4]`, storing into `[0x5DCF70+i*4]`; **crashes if the registry is empty** (§4.8) |
| `0x00419690` | registry getter: `esi = [eax*4 + 0x5DE098]`, then vector begin/end at `[esi+0x24]`/`[esi+0x28]`, element size `0x38` |
| `0x00418040` | **load-job executor**; first thing: `call 0x419E30`, and only proceeds when it returns non-zero; creates/uses the "package" object and registers it at `0x418799` (`mov [esi*4+0x5DE098], eax`); destroys it at `0x41857F`/`0x41858C` (`free` + `mov [esi*4+0x5DE098], 0`) |
| `0x00418730`… | package creation block: `EnterCriticalSection(0x5DE058)`, `HeapAlloc(0x5E1F28,0x44)`, `0x419110`, register; failure logs **`2308:Failed Alloc Package`** (`0x55951C`) and jumps to `0x41909B` |
| `0x00419110` | **package constructor** (writes `+4…+0x3C`, returns the object in `eax`) |
| `0x00419330` / `0x004193A0` | **`Cancel(id)`** (id en `ESI`): si el job actual (`[0x5BC918]`) tiene ese id pone el estado **3 = cancelado**, y marca `-1` el `[slot+4]` de todos los slots encolados con ese id (confirmado con Ghidra) |
| `0x00419D00` | **async loader thread body**: `EnterCriticalSection(0x5DE070)`, scan the ring for `[slot+4] != -1`, run `0x418040(slot[4], &slot[8])`, `LeaveCriticalSection`, loop; on "no work" sets `[0x5B3F82]=1` and calls `_endthreadex` |
| `0x00419530` | **"has the load finished?" poll** (called by the manager): `Sleep(16)`, `eax = 0x419430()`, returns **1** when the pump returns 0 or 2, else 0 |
| `0x00419430` | **loader pump** (main thread): compares ring head/tail, polls `[0x5BC914]` with `WaitForSingleObject(h,0)`, closes it, spawns a loader via `0x419CA0`, sets state 0 when head==tail |
| `0x00419CA0` | spawns one loader thread (`_beginthreadex(0x419D00)`), `SetThreadPriority(-2)`, `ResumeThread`, `[0x5BC91C]=1` |
| `0x00419E30` | **"job cancelled?"** (corregido en §9.1): returns 1 iff `[0x5BC914] != 0 && [0x5BC91C] == 3`.  `0x418040` **aborta** cuando devuelve 1 y sólo procede cuando devuelve 0 |
| `0x0041A820` | `CDirectSound::Init` (`DirectSoundCreate8`, `SetCooperativeLevel`, `CreateSoundBuffer`, `_beginthreadex`, `Run`) |
| `0x0041B2A0` | **audio mixer thread entry** (param `0x5DE128`) |
| `0x00404FB0` / `0x00405120` | `CSound::Create` / `CSound::Init` |
| `0x00522580` / `0x005226E0` / `0x00522480` | the game's **TextureRenderer** filter `Pause` / `Run` / `Stop` |
| `0x0045F1B0` | scene object constructor (allocated `0x3C` bytes by `0x42D9F8`) |
| `0x00439C80`, `0x0044D190` | further scene/managers constructed by `0x42D9F8` |
| `0x0045EB20` | **Title scene loader** – references `.\Data\Title.pak` (`0x567094`) — **never called yet** |
| `0x0045FF10` | **Warning scene** – `.\Data\Warning.pak` (`0x5671EC`) — **never called yet** |
| `0x00439A8C` | references `.\Data\KonamiLogo.pak` (`0x55C884`) — **never called yet** |

### 3.4 Subsystems implemented natively

* **kernel32** – files (Win32 disposition semantics: `OPEN_EXISTING=3 → O_RDWR`,
  `OPEN_ALWAYS=4 → O_CREAT|O_RDWR`), heaps (first-fit arena, `hdr_t`), `VirtualAlloc`,
  threads (own stack + TEB per thread, **completion flag `thread_arg_t.done`**),
  TLS in the TEB, code pages via `iconv` (CP932/936/949/950/1252),
  `GetProcAddress` (real export walk), `CreateFileMappingA`/`MapViewOfFile`,
  time, critical sections, events/semaphores.
  `win_host_path()` maps guest paths to the game dir: strips the drive letter,
  **strips an `nvmem\` component** (`E:\nvmem\testmode.bin` → `<dir>/testmode.bin`),
  and `\` → `/`.
* **user32** – one window (`HWND 0x00010001`), message queue,
  `RegisterClassExA` reads `lpfnWndProc` at **+8**, `CreateWindowExA` sends
  `WM_CREATE`, `WM_SIZE`, `WM_ACTIVATE(1)`, `WM_SETFOCUS`; close/ESC push `WM_QUIT`.
* **d3d9** – `IDirect3D9` (17) and `IDirect3DDevice9` (**119**, exact order);
  objects live in emulated memory with vtables of native thunks.
  `SetFVF` is now recorded; `DrawPrimitiveUP`/`DrawPrimitive` argument counts
  verified (`PrimitiveCount`, not vertices).
* **d3d9_objects** – texture(22)/surface(17)/buffer(14)/shader(5) with correct
  layouts; **`LockRect` implemented** (returns `Pitch`/`pBits`), **`GetSurfaceLevel`
  shares the texture's pixel buffer**, DXT1/3/5 + L8/A8 decode with a 64-entry
  cache (`texture_argb()`).
* **d3dx9** – real maths (`MatrixMultiply/Translation/Scaling/Rotation*/LookAtLH/
  PerspectiveFovLH/Transpose/Inverse`, `Vec3*`, `QuaternionSlerp`,
  `PlaneTransform`, `Vec2Hermite`), `D3DXCreateTexture` (creates a D3D texture),
  DDS header parse (`decode_dds` records the format; the DXT decode happens at
  sample time), `D3DXLoadMeshHierarchyFromXInMemory` returns `S_OK` with **NULL**
  outputs (the .x parser is not implemented).
* **dsound** – `IDirectSound8`(12) + `IDirectSoundBuffer8`(24); buffers hold
  emulated sample memory, `Lock/Unlock/Play/Stop`/volume/pan work, **audio silent**.
* **dshow** – full fake pipeline: `IGraphBuilder`(18), `IBaseFilter`(15),
  `IPin`(19), `IMediaControl`(16), `IMediaEvent`(13), `IMediaEventEx`(16),
  `IVideoWindow`(46), `IBasicAudio`(11), `IMediaPosition`(18),
  `IMediaEventSink`(4). `Render(pin)` → `EnumPins`+`Next`+`ReceiveConnection`
  with a hand-built `AM_MEDIA_TYPE`; `Run` → `Pause`+`Run` on every filter;
  `EC_COMPLETE` delivered; the video texture is created by us
  (`CreateTexture(device,1280,1024,1,0,0x16,0,&video+0x160,0)`).
* **avs** – Konami cabinet replacement (see §4.1) + touch panel input (§4.2).
* **render/platform** – `render.c` owns a software framebuffer (0xAARRGGBB),
  rasterises `XYZRHW|DIFFUSE|TEX1` triangles (stride 28) with wrap addressing and
  alpha blending; `platform_sdl.c` uploads it to a streaming texture each frame;
  `tests/platform_null.c` is a no-op backend.

* **FreeType font backend (`native/font.c`)** – `CreateFontA` opens a system
  TTF/OTF (DejaVu Sans by default, `OTOMEDIUS_FONT` override, `fc-match` +
  candidate paths, `/run/host/fonts` CJK fallback), sets the face to `h/4`
  pixels because `CTexFont` stores glyphs at 1/4 resolution, and implements
  `GetTextMetricsA` (56-byte TEXTMETRICA), `GetCharABCWidthsA` and
  `GetGlyphOutlineA` GGO_NATIVE.  Outlines are flattened to short
  `TT_PRIM_LINE` curves (`LINE_CHUNK=8`) because the game's rasteriser did not
  digest QSPLINE/CSPLINE or a single huge line curve.  All entry points use a
  recursive mutex because the async loader also logs through the same font.
* **x87 correction** – the recompiler emitted every `fxch st(i)` as
  `x86_fxch(cpu, 0)` (capstone gives two operands, `(st(0), st(i))`) and
  `x86_fpu_arith` wrote pop results to `st(b)=st(0)` instead of the
  destination `st(a)`.  Fixed in `tools/recomp/emit.py` and `native/x86rt.c`,
  then all 85 `rc_*.c` were regenerated.  The 2D setup at `0x401000` now
  yields `[0x5DB454]=640.0`, `[0x5DB458]=512.0`, `[0x5DB45C]=1.0`.

---

## 4. Parches y Workarounds Aplicados

### 4.1 Konami AVS libraries (`native/avs.c`) — all **cdecl** (`nargs = 0`)

| Function | Returns | Why |
|---|---|---|
| `ac_io_avs_boot` | `0` | cabinet I/O booted |
| `ac_io_begin` | **`1`** | the game tests `al` and aborts I/O init on 0 |
| `ac_io_update` | `0` | |
| `ac_io_pjei_update_control_status_buffer` | **`1`** | with 0, `0x413552` takes the "no data" branch and the game **never** calls `ac_io_pjei_get_control_status_buffer` — i.e. the cabinet input is frozen forever |
| `ac_io_pjei_get_control_status_buffer` | `0`, writes the 16-byte buffer | 16-bit switch word, active low: bit0 up, 1 down, 2 left, 3 right, 4..7 shot/bomb/option/special, 8 start, 9 coin |
| `ac_io_pjei_unlock_coincounter` | **`1`** | `0x413D3F` treats 0 as a hard error: logs `1183 ac_io_pjei_unlock_coincounter` through `0x403DB0`, which sets `[0x5DE220]=0xFFFFFFFF` and suspends every manager. This was one of the three errors that blocked the Konami boot. |
| `bms_time_get_clock_blocking(type, uint64_t *out)` | `0`, writes a real monotonic ms clock | The out pointer is the **second** argument (`arg32(1)`), not the first. Reading arg0 wrote to address 0/1 and left the caller's frequency at zero, so `0x44D400` divided by zero and the CRT raised an unhandled exception. |
| `ac_io_is_active2(dummy,&status)` | **`1`**, `*status = 0` | 0 made the detector read an uninitialised out-param and fall into I/O mode 7 (hard error) |
| `ac_io_pjei_req_secplug_check()` | **`1`** | 0 = rejected → I/O mode 7 |
| `ac_io_pjei_req_secplug_check_isfinished(&status)` | **`1`**, **`*status = 0x14`** | `0x4133D6` compares with 20; anything else → mode 7 |
| `ac_io_pjei_get_systemid/get_softwareid` | **`1`** + 20 bytes `"0123456789ABCDEFGHIJ"` | `0x41341C`/`0x413446` give up on 0 → mode 7 |
| `ac_io_icca_cardunit_init()` | **`1`** | needed to leave card state 0 |
| `ac_io_icca_cardunit_init_isfinished(1,&s)` | **`1`**, **`*s = 4`** | `0x41398C` accepts only 4 (or 5) |
| `ac_touchpanel_new` | real `emu_alloc(64)` pointer | the game keeps and dereferences it |
| `ac_touchpanel_send_check_diag` | **`1`** | request sent |
| `ac_touchpanel_check_wait_receive` | **`0`** | with 0 the game proceeds to `receive_check_diag` |
| `ac_touchpanel_receive_check_diag(h,&st)` | **`1`**, **`*st = 1`** | 0 loops back to T/P mode 1; the status byte must be non-zero |
| `ac_touchpanel_read_on[_direct]` | **`1`**, zeroed outs (or the touch position) | 0 means "panel did not answer" |
| `ac_touchpanel_caribrate_set` | `0` | accepted |
| `ac_touchpanel_polling` | `0` | no touches |
| `ac_touchpanel_renewal` | `0` | |
| `bms_init` | `0` | → boot state 1 |
| `bms_init_wait` | `0` | → `bms_eventlog_request_init` |
| `bms_eventlog_request_init` | `0` | → boot state 2 |
| `bms_is_active` | **`1`** | with 0 the boot machine jumps back to state 0 (infinite loop) |
| everything else in libacio/libbms/libtouchpanel | `0` | free play, no network, no card |

### 4.2 Touch panel input (pen / mouse / gamepad)

* `platform.h`: `touch_state_t { float x, y; int pressed, pressure; }` +
  `platform_read_touch()`; coordinates normalised 0..1 over the window.
* `platform_sdl.c`: `SDL_EVENT_PEN_DOWN/UP/MOTION` first (Wacom and friends),
  mouse motion/left button as fallback; once a pen has been seen it keeps
  priority (`g_touch.pen_seen`).
* `avs.c`: `ac_touchpanel_read_on[_direct]` report raw panel coordinates
  (0..1280 / 0..1024) in arg1/arg2, pressure in arg3, pressed flag in arg4.
  Keyboard/gamepad and the panel are read independently, so any combination
  works (keyboard+gamepad, keyboard+tablet, all three).
* `tests/platform_null.c` reports "never touched".

### 4.3 D3D9 workarounds

* `d9_GetDeviceCaps`: full realistic `D3DCAPS9` (SM 3.0 `0xFFFE0300`/`0xFFFF0300`,
  `MaxVertexShaderConst 256`, `MaxStreams 16`, `MaxPrimitiveCount 0x7FFFFF`,
  `MaxVertexIndex 0xFFFFFF`, `MaxTextureWidth/Height 8192`,
  `MaxSimultaneousTextures 8`, `DevCaps HWTRANSFORMANDLIGHT`).
  **pCaps is `arg32(3)`** (`this, Adapter, DeviceType, pCaps`) — reading arg 2
  made the game reject every device type and never call `CreateDevice`.
* `GetAdapterIdentifier` → `arg32(3)`; `EnumAdapterModes` → `arg32(4)`.
* `d9_CreateDevice`: `ppDevice = arg32(6)`, size from `D3DPRESENT_PARAMETERS`
  (`BackBufferWidth +0`, `Height +4`, `Windowed +32`).
* `dev_Present` returns 0 and calls `render_present()`; `TestCooperativeLevel`
  returns 0.

### 4.4 Renderer bring-up (`render.c`, `platform_sdl.c`, `d3d9_objects.c`)

* Software rasteriser for the only pipeline the game uses:
  **FVF `0x144` = `XYZRHW|DIFFUSE|TEX1`, stride 28**, `D3DPT_TRIANGLESTRIP`
  with `PrimitiveCount = 2` (4 vertices = one quad), source `0x5BE808`.
* Nearest sampling with WRAP addressing (`u = 6.0` samples texel 0), diffuse
  modulation, source-alpha blending into a `0xAARRGGBB` framebuffer.
* `render_framebuffer()` exposes the buffer; `platform_sdl.c` creates/updates a
  streaming texture and `SDL_RenderTexture`s it.
* Frame statistics every 120 presents:
  `render: frame N: clear=… draw=… up=… verts=… fvf=… stride=… vb=… tex0=… bbox=… onscreen=…`.
  **Measured after the boot: 25 `DrawPrimitiveUP` per frame, `draw=0`
  (never `DrawPrimitive`), bbox `(-631,-5)-(12,0)`** — the UI is drawn above the
  top edge (see §5).
* `dev_SetFVF` now records the FVF (`render_set_fvf`).
* `D3DXLoadMeshHierarchyFromXInMemory` **must** write its two out-params to NULL
  (or return a real hierarchy): returning `S_OK` with untouched stack garbage
  made the game treat it as an `ID3DXMesh` and `call [vtbl+0x28]` → jump to
  `0x10A97` (crash in the loader thread).

### 4.5 `oto.ini` loader (`native/config.c`)

The game's tuning values live in a table at **`0x5BE328`** (`{name, target, type}`
× 97, type 1 = byte, 3 = u16, 6 = u32, 7 = float). None of the targets are
file-backed, so the image loads them as **zero** — and `wSortDrawCmn2DMax = 0`
makes the first `CSortData` registration overflow, which logs error **1115**
(`0x409430` → `'1115 CSortData::Regist over %d:%s'`) and makes `0x403DB0` set
`[0x5DE220] = 0xFFFFFFFF`, suspending every manager.

`game_config_load()` parses `oto.ini`/`otomedius.ini` (`set <name> <value>`),
walks the game's own table (so keys/widths always match the binary) and stores
into the targets. It runs twice: before the entry point and again right after the
game's own `memset(0x5DB130,0,0x148)` at `0x401EC6` (hooked at `0x401ED8` through
`game_config_after_reset()`), because the memset would discard it.
Log: `config: oto.ini: 85 values applied, 0 unknown`.

### 4.6 Integrity check bypass (`native/patches.c`)

`ChkSum.bin` is a manifest of 191 files with their MD5; `0x40ED00` verifies them
in order and aborts on the first mismatch (event 4, manager destroyed).
**This dump is a later revision: 22 of 191 files do not match** (first:
`Data/CardEntry.pak`). So `x86_patch_call()` replaces `0x40ED00` with
"verified". `OTOMEDIUS_CHKSUM=1` restores the original behaviour.

> This is why the game appeared to "load 10 paks alphabetically and stop": it was
> **hashing** them, not loading them.

### 4.7 Threads (`win32_kernel32.c`, `main.c`)

* `ExitThread` **must not** call `pthread_exit()` from inside the dispatcher: it
  strands the thread inside glibc's cleanup (`pthread_tryjoin_np` returns
  `EBUSY` forever). Each emulated thread arms a `jmp_buf` in
  `x86_thread_begin()` and `x86_thread_exit()` longjmps to it; the thread then
  returns through the trampoline.
* Thread completion is tracked by **`thread_arg_t.done`** (set by the thread
  itself), not by `pthread_tryjoin_np`. `WaitForSingleObject(thread, …)` honours
  the timeout: `INFINITE` joins, `0` returns `WAIT_TIMEOUT` unless done, anything
  else polls with a deadline.
* `CloseHandle` keeps `H_THREAD` entries as zombies so the game can keep polling
  the same handle value.
* Async loader ring (`0x5DCF98`): free slots **must** be `-1`. It lives in
  uninitialised `.data`, so `game_state_init()` writes `-1` into all 16 slots at
  startup. Without it the loader "processed" empty slots, failed and stopped in
  state 4.
* **Obsoleto (ver §9.1):** el "handshake que nunca completa" era un bug nuestro
  (`thread_arg_t.done` sin inicializar).  El guard de `0x419530` ya no está
  activo por defecto; `OTOMEDIUS_LOADER_GUARD=1` lo reactiva.

### 4.8 Finalise guard (`native/patches.c`)

`0x414920` reads the package registry `0x5DE098` and calls `[[entry]+0x24]`.
The registry is **transient**: the load job registers a package at `0x418799`
and destroys it at `0x41858C`, so by the time the load reports done it is empty
and the call goes through NULL. `x86_patch_call()` returns 1 (skip) when
`[0x5DE098] == 0`; the original path still runs when a package is live.

### 4.9 Recompiler / emitter workarounds

* Function bodies that end without a terminator emit
  `cpu->eip = <next>; return;` ("falls through into the next function").
* `call`/`jmp` with a computed target go through `x86_jump()`, which aborts with
  a diagnostic when the target is 0.
* `jmp [IAT]` (import thunks) use `x86_native_tail()` = invoke the native
  function, pop its args **and the pushed return address**, then set
  `cpu->eip = ret`.
* **Vtable argument counts:** `STDMETHOD(Name)(THIS)` ⇒ **1**;
  `STDMETHOD(Name)(THIS_ args…)` ⇒ **args + 1**.
* `--seed` / `recomp/seeds.txt` for entry points reachable only through computed
  pointers (7 today).
* `OTOMEDIUS_BADTARGET=skip` turns an unknown target into a no-op that "returns",
  so one run reports **every** missing function (`[missing] <addr> (caller <addr>)`).

### 4.10 GDI font path — legacy synthetic fallback (`win32_misc.c`)

The original synthetic path is still compiled in as a fallback (used when
FreeType/`OTOMEDIUS_NO_FREETYPE` is unavailable): `CreateFontA` stores the
requested height; `GetTextMetricsA` and `GetCharABCWidthsA` return coherent
metrics; `GetGlyphOutlineA` implements `GGO_NATIVE` (a closed rectangle
contour in `TTPOLYGONHEADER`/`TTPOLYCURVE`, `cb = 52`, 16.16 fixed point,
+y upwards) and `GGO_BITMAP` (1bpp box).  The real path is §4.11.  Font face
names are logged converted **CP932 → UTF-8** (Shift-JIS:
`ＤＦＧ中太丸ゴシック体`).

---


### 4.11 FreeType font backend (`native/font.c`, `font.h`)

`win32_misc.c` now calls into `font.c` for all GDI font work; the synthetic
rectangle path remains only as a fallback.  Key points:

* Font selection: `OTOMEDIUS_FONT` → `fc-match sans` → candidate list
  (`/usr/share/fonts/dejavu/DejaVuSans.ttf`, `/usr/share/fonts/truetype/…`,
  `/usr/share/fonts/gnu-free/FreeSans.ttf`, ipagothic, Noto CJK,
  `/run/host/fonts/…`).  DejaVu is preferred because TrueType has no cubics;
  CJK can be forced with `OTOMEDIUS_FONT`.
* `FT_Set_Pixel_Sizes(face, 0, (h+3)/4)` — `CTexFont` stores glyphs at 1/4
  of the screen size (h=24 → 6 px, h=48 → 12 px).  This is the current
  working hypothesis; §5.2 explains why the atlas is still dense.
* `GetTextMetricsA` writes exactly **56** bytes (`sizeof(TEXTMETRICA)`), see
  §4.12.  `GetCharABCWidthsA` currently writes the game's compact 6-byte ABC
  layout (`a,b,c` as 16-bit) at `abc + i*6`.
* `GetGlyphOutlineA` GGO_NATIVE: `FT_Load_Glyph(FT_LOAD_NO_BITMAP |
  FT_LOAD_NO_HINTING)`, fills `GLYPHMETRICS` (`gmBlackBoxX/Y`,
  `gmptGlyphOrigin`, `gmCellIncX`), flattens conics/cubics to short line
  segments (`LINE_CHUNK=8`) and emits `TTPOLYGONHEADER` (cb, dwType=24,
  pfxStart) + `TTPOLYCURVE` (wType=1, cpfx, points) in 16.16 fixed point.
  Contours are explicitly closed.  The size query (`cb=0`) builds the same
  buffer and returns its size.
* Thread safety: `pthread_mutex_t` with `PTHREAD_MUTEX_RECURSIVE`, because
  the loader thread logs through `0x405120` while the main thread draws.
* The 7th `GetGlyphOutlineA` argument (`gm2=&[font+0x3A]`) is **not**
  mirrored: the game pre-fills those fields as UV/scale offsets.  See §5.2.

### 4.12 `TEXTMETRICA` overrun (`win32_misc.c`)

The old `g_GetTextMetricsA` did `memset(MEM + tm, 0, 64)`.  `TEXTMETRICA` is
**56** bytes; the game's `CTexFont` passes a `0x3C`-byte local at `ebp-0x3C`.
The 64-byte clear wrote over the saved EBP, so `0x409C00` returned with
`ebp=0`; the next call used `[ebp+0xC]` = NULL and died with
`no recompiled code at 00000000` while consuming the synthetic GGO_NATIVE
contour.  Fixed to `memset(..., 0, 56)`.  This was the first crash in the
font path.

### 4.13 Loader/package guards (`native/patches.c`)

* `0x414920` (finalise/commit): the original guard only skipped when the
  registry `0x5DE098` was NULL.  It now also skips when the package exists
  but its first record (`[pkg+0x24]+0x20`) is NULL or has no vtable, because
  that also ends in a call through address 0.
* `0x418040` (load-job executor, cdecl, 2 args): if the job id (`[esp+4]`) is
  `>= 4` (the package registry has 4 entries), return `eax=0` so the worker
  treats the job as not performed; this avoids the out-of-range index `0x10`
  seen in the loader ring.
* `0x418190` (package lookup/commit, cdecl, 1 arg): **skipped entirely**.
  The loader's C++ record vector is not in a state the game's code can walk
  (the same reason `0x414920` is skipped); the file mapping already happened
  in `0x418040`.  Log: `config: skipping the package commit step`.
* These guards keep the port stable without the loader crash; they are not
  the final loader fix.

### 4.14 Adapter mode / 1280×1024 / SDL window

`d9_EnumAdapterModes` used `g_dev.width ? g_dev.width : 1024`; before the
device exists `g_dev.width` is 0, so it advertised 1024×768 while the game's
window constants are 1280×1024.  The game then created a 1024×768 backbuffer
inside a 1280×1024 window, which put all 2D UI coordinates off-screen.
Now it advertises `render_width()/render_height()` and `render.c` defaults to
**1280×1024**.  `platform_set_size` calls `SDL_SetWindowSize` so the SDL
window follows the device resolution.  Log: `CreateDevice(1280x1024)`.

### 4.15 Texture `LockRect` default + UV cell remap

`IDirect3DTexture9::LockRect` now succeeds by default (the game uploads its
font atlases through it).  `OTOMEDIUS_TEXTURES=0` restores the old
always-fail behaviour for bisecting.  The atlas upload is visible as:

```
d3d9: tex LockRect(60040490) 512x64 -> 600404d0
```

`render.c` also has a heuristic for the game's **cell-index UVs** (u=6.0,
v=4.0): when the UVs are out of the normalised range with a sub-cell span,
it maps `u_norm = floor(u)*cell_u + frac(u)` (and the same for V), where
`cell_u/cell_v` are the max-min of the quad's UVs.  This is required to reach
the intended atlas cell, but §5.2 shows the atlas packing still disagrees with
the 48×6 cell size.

### 4.16 Konami boot: the three errors that suspended every manager

The boot/attract manager (`0x4547E0`) and the KonamiLogo manager (`0x4393A0`,
update `0x439550`, draw `0x4395B0`) were alive but never ran because
`[0x5DE220]` (the manager enable mask) was `0xFFFFFFFF`.  Three independent
errors set it through `0x403DB0`:

1. **Shader compile** — `D3DXCreateEffect` is a stub returning `E_FAIL`, so the
   asset loader logs `2302:Failed Create Shader` (`0x559580`) and
   `2306:Failed Build` (`0x5594C4`).  `patches.c` now intercepts `0x403DB0`
   and ignores exactly those two format strings.
2. **AVS coin counter** — `ac_io_pjei_unlock_coincounter` returned 0; `0x413D3F`
   logs `1183 ...` (`0x5587D4`).  The stub now returns 1 (see §4.1).
3. **BMS clock** — `bms_time_get_clock_blocking` read the wrong argument and
   returned 0, so `0x44D400` divided by zero.  The stub now uses `arg32(1)` and
   a real `CLOCK_MONOTONIC` millisecond counter (see §4.1).

Additional guards in `patches.c`:

* `0x402840` (save `bookkeep.bin`) is skipped: the save path builds
  `E:\nvmem\bookkeep.bin`, the host-path conversion corrupts the name, and the
  following `CreateFileA` gets a garbage string and raises an unhandled
  exception.  Bookkeeping is not needed for the logo.
* `0x4547E0` (boot/attract update) forces `[0x5E2136]=1` (cabinet I/O ready),
  skips the "wait for I/O" state (`[mgr+0xC]=1`), and clears the boot/attract
  manager slot `0x5DE258` when the state machine is at state 1 / sub-state 1.
  Without this the state machine never advances past state 0.
* `OTOMEDIUS_KONAMI=1` (in `render.c`) bypasses `0x4547E0` entirely and calls
  `0x439920` (set the logo string) + `0x4395B0` (draw) on the KonamiLogo
  manager at `[0x5DE23C]` every frame.  This draws the logo quad but not the
  real texture (see §5.4).

## 5. Estado Actual y Bug/Bloqueo Activo

### 5.1 Lo que funciona (verificado en local y en Flatpak)

El port **arranca de punta a punta** y es **estable durante 30 s** con
`timeout 30` (`exit 124`, **0** líneas `no recompiled code`). El log del
Flatpak (build `a2ec90e0…`, antes de FreeType) confirma:

```
loader: image 00400000-005e9000 entry 00530e2b
imports: 300 bound, 0 missing
config: oto.ini: 85 values applied, 0 unknown
d3d9: Direct3DCreate9(32) -> 60000090
d3d9: GetDeviceCaps(...)
d3d9: CreateDevice(1280x1024) windowed=0
user32: CreateWindowExA(1280x1024) wndproc=00401bb0
dsound: DirectSoundCreate8 -> 60040420
dshow: filter graph created (600e0640)
gdi32: CreateFontA(h=48 face=ＤＦＧ中太丸ゴシック体)
gdi32: CreateFontA(h=28 face=ＤＦＧ中太丸ゴシック体)
gdi32: CreateFontA(h=24 face=ＤＦ中太丸ゴシック体)
d3d9: tex LockRect(60040490) 512x64 -> 600404d0
render: frame 360: clear=1 draw=0 up=25 verts=50 fvf=000144
        tex0=60040490 bbox=(-303352,8)-(2926,319040) onscreen=5
platform: Esc pressed
```

Estado de los globals tras el boot (verificado con `OTOMEDIUS_WATCHMEM`/
`OTOMEDIUS_WATCHCHG`):

| Global | Valor / significado |
|---|---|
| `[0x5E2027]` | `0xFF` = carga terminada |
| `[0x5E1F96]` | `2` = boot machine “ready” |
| `[0x5E1FA5]` / `[0x5E1FA6]` / `[0x5E1FA7]` | `5` / `3` / `2` |
| `[0x5DE220]` | `0` (ningún manager suspendido) |
| `[0x5DE228]` | puntero al manager de escena vivo |
| `[0x5DB454]` / `[0x5DB458]` | `640.0` / `512.0` (centro 2D correcto) |
| `[0x5DB45C]` / `[0x5DB460]` | `1.0` / `1.0` |
| `[0x5E1F88]` / `[0x5E1F8C]` | objetos `CTexFont` 512×64 / 512×256 |

Las texturas reales están **activadas por defecto** (`OTOMEDIUS_TEXTURES=0`
restaura el comportamiento antiguo en el que `LockRect` siempre falla). El
atlas 512×64 (`0x60040490`) **se construye y se sube**; el de 512×256
(`0x60060570`) también existe. El de la fuente local de escena es
`0x605e0a40` (1024×512).

La escena dibuja 25 `DrawPrimitiveUP` por frame. Los 16 quads de texto están
en `(448,480)-(832,536)` con `tex0=0x60040490`; el marco de la consola de
depuración es el rectángulo rojo `(372,476)-(907,547)`. Los otros 4 quads
tienen coordenadas enormes (`bbox=(-303352,8)-(2926,319040)`) y quedan fuera
de pantalla; son un camino de dibujo aparte, pendiente de revisar.

### 5.2 Bloqueo de texto: `GGO_GRAY8_BITMAP` vs `GGO_NATIVE` (causa raíz)

**El juego NO pide `GGO_NATIVE`.**  La llamada real es:

```
gdi32: GetGlyphOutlineA(ch=101 fmt=6 gm=009fee24 gm2=1003d66a cb=0 buf=00000000)
gdi32: GetGlyphOutlineA(ch=101 fmt=6 gm=009fee24 gm2=1003d66a cb=1160 buf=1003d6e8)
```

`fmt=6` es **`GGO_GRAY8_BITMAP`** (las constantes Win32 son
`GGO_BITMAP=1`, `GGO_NATIVE=2`, `GGO_GRAY2_BITMAP=4`,
`GGO_GRAY4_BITMAP=5`, `GGO_GRAY8_BITMAP=6`).  Nuestro `native/font.c`
comprueba `format & 2` y devuelve un **outline** (`TTPOLYGONHEADER` /
`TTPOLYCURVE`), pero el juego lo lee como un **bitmap de 8 bpp** en la rama
`0x409E5E` de `0x409D20` (la rama se elige con `[font+0x38]==1`, que es el
caso: `f38=1 f39=0`).  Los bytes del outline se interpretan como alpha →
**ruido/rayas**.  Esta es la causa raíz, no el empaquetado.

Datos medidos con `OTO_PACKDBG=1` (instrumentación temporal en
`.work/gen/rc_002.c`, no en el repo):

```
pack: font=1003d630 f38=1 f39=0 penX=0 penY=0 gm=009fee24
      bbX=7 bbY=7 orgX=0 orgY=6 incX=7 incY=0 gm10=0 buf=1003d6e8
pack2: arg2=009fee68 *arg2=4096 arg3=009fee70
       a3[0]=0000000e a3[1]=0000000c a3[2]=00000003 a3[3]=00000000
pack3: font=1003d630: 605e0a40 0f010000 0f000002 1003d618
       00000017 00000030 00010001 0dc00001 3f800000 3f800000 ...
```

* El objeto de fuente es `0x1003d630`; su textura es `0x605e0a40`
  (**1024×512**, no 512×64).  `[font+0x38]=1` (modo gray8),
  `[font+0x39]=0`.
* `arg2` es un struct de 2 dwords: `[0]=4096` (stride de fila en bytes,
  1024 px × 4) y `[1]=0x1003d6e8` (puntero al buffer del atlas).
* `arg3` es el `TEXTMETRICA` (tmHeight=14, tmAscent=12, tmDescent=3).
* `gm` es el `GLYPHMETRICS` de la API: `bbX=7 bbY=7 orgX=0 orgY=6
  incX=7 incY=0`.  El juego lee el avance de `gm+0x10` en `0x409F5E`
  (`movsx edx, word ptr [esp+0x30]`), que vale 0; el `gmCellIncX` real
  está en `gm+0x0C`.  El local `gm` tiene 20 bytes (5 dwords puestos a 0
  en `0x409B50`), así que el juego espera un struct de 20 bytes con el
  avance en `+0x10`.  Esto es un **problema secundario**: aunque se
  arregle, el atlas seguirá siendo ruido mientras se devuelva un outline
  en vez de un bitmap gray8.

**Fix correcto (pendiente de aplicar):**

1. En `native/font.c`, implementar de verdad `GGO_GRAY8_BITMAP`:
   renderizar el glifo con FreeType a un bitmap de cobertura, escribir
   `stride = (width + 3) & ~3` bytes por fila, un byte por píxel
   (0..255), y rellenar `gmBlackBoxX/Y`, `gmptGlyphOrigin`,
   `gmCellIncX/Y`.  `GGO_BITMAP` (1) es 1 bpp MSB-first con
   `stride = ((width + 31) / 32) * 4`.  `GGO_NATIVE` (2) solo para
   `format == 2`.
2. En `native/win32_misc.c`, `g_GetGlyphOutlineA` debe enrutar por
   `format` exacto, no por `format & GGO_NATIVE`.
3. Escribir también el avance en `gm+0x10` (además de `gm+0x0C`) para
   que `0x409F5E` avance `penX`; el juego lee `gm+0x10`.
4. Validar con `OTOMEDIUS_TEXDBG=1`: el atlas debe ser **disperso**
   (glifos separados) y el frame debe mostrar “NOW INITIALIZING”
   legible en `(448,480)-(832,536)`.

El remapeo de UVs de celda en `native/render.c` sigue siendo necesario
para llegar a la celda correcta, pero **no** es la causa del ruido:

```c
cell_u = maxu - minu;
cell_v = maxv - minv;
cell_uv = (maxu >= 1.5f || maxv >= 1.5f) &&
          cell_u > 0 && cell_v > 0 && cell_u < 1.0f && cell_v < 1.0f;
if (cell_uv) {
    vt[i].u = floorf(vt[i].u) * cell_u + (vt[i].u - floorf(vt[i].u));
    vt[i].v = floorf(vt[i].v) * cell_v + (vt[i].v - floorf(vt[i].v));
}
```

Con `u=6.0`, `cell_u=0.09375` da `u_norm=0.5625` (texel 288) y con
`6.09375` da `0.65625` (texel 336): un span de 48 texels.  La tabla de
fuente (`0x5BE7CC`/`0x5BE7E8`) tiene `+0x10=0.09375`, `+0x14=0.09375`
(48/512 = 6/64), y los glyph records son `N=(6,4)`, `O=(7,4)`,
`W=(5,5)`, `I=(1,4)`, `T=(2,5)`, `A=(3,3)`, `L=(4,4)`, `Z=(8,5)`,
`G=(9,3)`.  El atlas de 1024×512 usa `0.09375 = 96/1024` en U y
`0.09375 = 48/512` en V, así que la celda lógica es **96×48 px** para
ese atlas (no 48×6).  Confirmar con el atlas real antes de tocar el
remapeo.

### 5.3 Problemas secundarios y estado del árbol

* **Loader/package commit**: el worker del ring pasa `slot[+4]` como índice
  de paquete; en nuestra emulación llega `0x10` (16), fuera de la tabla de 4
  entradas `0x5DE098`, y `0x418190` acaba en un `call` a vtable inválida.
  Añadidos guards: `0x418040` devuelve `al=0` si `id>=4`; `0x418190` se salta
  siempre (el commit del paquete). El guard de `0x414920` se amplió para
  saltar también cuando el registro existe pero no tiene un record vivo.
  Esto mantiene el port estable; el commit real habrá que entenderlo.
* **Escena real**: solo se ha cargado `Once.pak`. `0x45EB20` (Title),
  `0x45FF10` (Warning) y `0x439A8C` (KonamiLogo) **nunca se han llamado**.
  “NOW INITIALIZING” es la consola de depuración, no la pantalla del juego.
  El boot de Konami **sí** corre ahora (ver §5.4), pero la textura real de
  `KonamiLogo.pak` aún no se carga.
* **Japonés**: la fuente por defecto es DejaVu Sans (ASCII). Para CJK usar
  `OTOMEDIUS_FONT=/ruta/NotoSansCJK.ttc` o similar; el runtime tiene DejaVu
  en `/usr/share/fonts/dejavu/DejaVuSans.ttf`, y `/run/host/fonts` puede
  aportar Noto CJK si el sandbox lo permite.
* **4 quads con coordenadas enormes**: bbox `(-303352,8)-(2926,319040)`.
  Están fuera de pantalla y no bloquean, pero conviene identificar el draw
  (posiblemente el marco de la consola con una escala mal calculada).

### 5.4 Boot de Konami: estado actual

Tras los parches de §4.16, el juego **ya no crashea** y los managers se
actualizan cada frame:

```
[watch] 00439550 #1 ... ecx=1003fa78 edx=0055c870   ; KonamiLogo update
[watch] 004547e0 #1 ... ecx=1003fbe0 edx=00563db0   ; boot/attract update
```

* `0x439550` (KonamiLogo update) y `0x4547E0` (boot/attract update) se
  llaman cada frame.
* El **quad del logo se dibuja**: `up=2..4 verts=4..8 fvf=000044
  bbox=(0,0)-(2312,1024)`.  Sale como **rectángulo blanco** porque la
  textura real de `KonamiLogo.pak` no está cargada (`tex0=00000000`).
* El state machine del boot/attract manager (`0x4547E0`) avanza de
  `state=0` a `state=1`, y de `sub=0` a `sub=1` y `sub=8`, pero se queda
  atascado en **`state=1 sub=8`** (`0x454A74`).  Ese bloque asigna 0xC
  bytes y llama al constructor `0x429210`, que registra en `0x5DE258`;
  en nuestra ejecución `[0x5DE258]` sigue a 0 y el sub-estado no avanza
  a 9.  Hay que averiguar si `0x4142B0` devuelve 0 o si `0x429210` no
  llega a registrar.
* Con `OTOMEDIUS_KONAMI=1` se salta `0x4547E0` y se llama directamente
  `0x439920` (fija el string) + `0x4395B0` (dibuja) sobre el manager de
  `[0x5DE23C]`.  Eso dibuja el quad pero **no** la textura real.
* El atlas de fuente del logo (`0x607e0b20`, 2048×128) se crea y se
  bloquea (`LockRect`), pero el logo real es una textura de
  `KonamiLogo.pak`, no el atlas de texto.

Ficheros modificados/nuevos en el working tree (checkpoint sin commit al
escribir esto; el commit se hará al cerrar la sesión):

```
M native/avs.c        # coin counter returns 1; BMS clock arg/real time
M native/patches.c    # shader error ignore, bookkeep skip, boot state force
M native/render.c     # OTOMEDIUS_KONAMI experimental hook
M docs/HANDOVER.md    # this document
```

Los cambios de la sesión anterior (`font.c`, `font.h`, `d3d9*.c`,
`win32_misc.c`, `x86rt.c`, `emit.py`, manifiesto) ya están en el commit
`aff81d6`.

`docs/HANDOVER.md` y `README.md` también se actualizan en el checkpoint.

Artefactos locales (fuera del repo): `.work/gen` regenerado (85 ficheros con
`--chunk 60`), `.work/obj` reconstruido, `.work/freetype_include` y
`.work/freetype_lib` extraídos de FreeType 2.13.2 para el contenedor del
agente. El Flatpak regenera **42** ficheros `rc_*.c` porque el manifiesto no
pasa `--chunk 60`; no es un bug, pero explica la diferencia con los 85
locales.

## 6. Plan de Acción Inmediato

### Paso 1 — Implementar `GGO_GRAY8_BITMAP` (texto legible)

El juego pide `fmt=6` (`GGO_GRAY8_BITMAP`), no `GGO_NATIVE`.  En
`native/font.c`:

1. Añadir una función `font_glyph_bitmap(handle, ch, format, gm, cb, buf,
   &size)` que:
   - `FT_Load_Glyph(face, gi, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL)`.
   - Para `GGO_GRAY8_BITMAP` (6): `stride = (bitmap.width + 3) & ~3`,
     `size = stride * bitmap.rows`, copiar `bitmap.buffer` (un byte por
     píxel, 0..255).
   - Para `GGO_BITMAP` (1): `stride = ((width + 31) / 32) * 4`, empaquetar
     1 bpp MSB-first.
   - Rellenar `gmBlackBoxX/Y`, `gmptGlyphOrigin`, `gmCellIncX/Y` y
     **también** `gm+0x10` con el avance (el juego lee `gm+0x10` en
     `0x409F5E`).
2. En `native/win32_misc.c`, `g_GetGlyphOutlineA` debe enrutar por
   `format` exacto: `2` → outline (el código actual), `6` → gray8,
   `1` → 1bpp.  No usar `format & GGO_NATIVE`.
3. Recompilar y validar:
   ```sh
   sh /tmp/opencode/build.sh font.c win32_misc.c
   cd ../.work/oto && OTOMEDIUS_TEXDBG=1 OTOMEDIUS_DUMP_FB=/tmp/opencode/t.ppm \
     OTOMEDIUS_DUMP_AT=300 ../obj/otomedius-recomp .
   ```
   El atlas debe ser **disperso** y el frame debe mostrar “NOW
   INITIALIZING” legible en `(448,480)-(832,536)`.

### Paso 2 — Desatascar el boot/attract manager (logo real)

El state machine `0x4547E0` se queda en `state=1 sub=8` (`0x454A74`).
Ese bloque hace:

```
push 0xc; push 0x5e1f28; call 0x4142b0   ; alloc 12 bytes
cmp eax, edi; je 0x454a9a                ; if alloc failed, skip ctor
mov esi, eax; call 0x429210              ; ctor, registers at 0x5DE258
add byte [ebp+0xd], 1                    ; -> sub-state 9
```

Investigar con `OTO_BOOTDBG=1` (print temporal en `.work/gen/rc_017.c`):

1. ¿`0x4142B0` devuelve 0?  Si sí, el heap local está agotado o el
   descriptor `0x5E1F28` está corrupto.
2. ¿`0x429210` registra en `0x5DE258`?  El constructor comprueba
   `[0x5DE258] == 0`; si no, llama a `0x403DB0` con
   `"1044 Overlap create manager ID:%d"` y pone la máscara a
   `0xFFFFFFFF`.  Nuestro parche limpia `0x5DE258` solo en
   `state=1 sub=1`; puede que en `sub=8` vuelva a estar ocupado.
3. Si el constructor registra, el sub-estado debería pasar a 9 y luego
   a 10/11, que crean los managers de la escena y cargan
   `KonamiLogo.pak`.  Verificar con
   `OTOMEDIUS_WATCH="439a8c,4397c0,41e280,4395b0"`.

### Paso 3 — Validar el logo y la escena real

Con el boot desatascado:

```sh
OTOMEDIUS_LOG=1 OTOMEDIUS_WATCH="439a8c,45eb20,45ff10" \
  OTOMEDIUS_DUMP_FB=/tmp/opencode/logo.ppm OTOMEDIUS_DUMP_AT=600 \
  ../obj/otomedius-recomp .
```

Buscar `CreateFileA(./Data/KonamiLogo.pak)`, `Title.pak`, `Warning.pak`.
El logo real es una textura de `KonamiLogo.pak`; el quad ya se dibuja
(`bbox=(0,0)-(2312,1024)`), pero `tex0=00000000`.  Cuando la textura se
cargue, el frame debe mostrar el logo de Konami, no un rectángulo blanco.

Después: renderer de sprites reales (`0x40F780`/`0x40FA44`), audio
(`dsound` → PulseAudio), y carga de pak más rápida (buffer de lectura en
`k_ReadFile`).

---

## 7. Sesión 2026-11-16: bezel, vídeo de introducción y audio

### 7.1 Bezel de la cabina y modo ventana (`native/platform_sdl.c`)

El dump trae `bezel.png` (1920x1080, RGBA) con el arte de los costados y un
**hueco de pantalla transparente**. La capa SDL:

* Carga `bezel.png` del directorio del juego (o `OTOMEDIUS_BEZEL=path`;
  `OTOMEDIUS_BEZEL=0` lo desactiva) antes de crear la ventana.
* Detecta el hueco por el canal alfa: columnas transparentes en (casi) todo el
  alto y, dentro, filas transparentes. Para este arte: **hueco
  `(288,0)-(1631,1079)` = 1344x1080**. Si el hueco fuese opaco dibuja el arte
  debajo; aquí es transparente y el arte va **encima** (`overlay`).
* Ventana: **fullscreen por defecto**; `F11` alterna y al volver a ventana pone
  **1366x768** centrado. `OTOMEDIUS_WINDOWED=1` arranca en ventana.
* Composición: el arte se escala manteniendo su aspecto y el frame del juego
  (1280x1024) se encaja en el hueco manteniendo el suyo (1344x1075, centrado).
* Ratón/lápiz se mapean al **rectángulo del juego** (no a la ventana), con
  corrección HiDPI (`SDL_GetWindowSize` vs `SDL_GetWindowSizeInPixels`).
* Diagnóstico: `OTOMEDIUS_BEZEL_DUMP=file.ppm` (+ `..._AT=n`) lee el
  backbuffer compuesto con `SDL_RenderReadPixels` **antes** de `Present`.

`flatpak/otomedius-recomp-launch` copia `bezel.png` al `GAME_DIR` si falta
(instalaciones ya existentes) y pone delante
`/usr/lib/x86_64-linux-gnu/codecs-extra/lib` en `LD_LIBRARY_PATH`.

### 7.2 Vídeo: DirectShow real + decodificación con ffmpeg

El juego reproduce `Data/OTOMEDIUS_OP_f.wmv` (WMV3 640x512 30 fps, 60 s,
audio WMAv2) a través de un grafo DirectShow **falso** (`native/dshow.c`) y de
su propio filtro `TextureRenderer` (clase en `0x4175C0`).

Cadena medida (direcciones del binario original):

| Dirección | Papel |
|---|---|
| `0x417480` | sonda de arranque: abre el grafo y lo cierra (no debe decodificar) |
| `0x4174B0` | abre el "movie wrapper" (0x1C bytes en `[0x5DCF88]`) |
| `0x417A80` | construye el grafo: `CoCreateInstance`, `AddFilter(renderer)`, `AddSourceFilter`, `Render` |
| `0x417D90` | cierra y libera el wrapper |
| `0x417590` | cierra el wrapper global (`[0x5DCF88]=0`) |
| `0x43F280` | ctor del **manager del vídeo** (vtable `0x55E904`), registra en `0x5DE258` |
| `0x43F370` | update del manager: estados 0..4 (`0x43F4CC`), abre el grafo, fade, temporizador 3570 |
| `0x43F4E0` | draw del manager: encola `0x43F540` en la lista `[0x5E1F9C]` |
| `0x43F540` | callback de dibujo del vídeo (quad con la textura) |
| `0x4175C0` | ctor del `TextureRenderer` (objeto con IBaseFilter en +0xC, IPin, vtables `0x55926C/0x559334/0x559374/0x55938C`) |
| `0x4176E0` | `SetMediaType` del renderer: exige `FORMAT_VideoInfo`, `MEDIATYPE_Video` y **`MEDIASUBTYPE_RGB24`** (GUID en `0x55917C`) |
| `0x417750` | crea la textura del vídeo (1280x1024 X8R8G8B8) al aceptar el media type |
| `0x527110` | `ReceiveConnection` del pin de entrada del renderer |
| `0x521390` / `0x521400` | métodos del pin que delegan en el renderer (`SetMediaType`, `NotifyAllocator`) |
| `0x52DA30` | copia el media type con **`CoTaskMemAlloc`** (si devuelve NULL -> `E_OUTOFMEMORY`) |

Cambios en `native/dshow.c`:

* `AddSourceFilter` recibe el nombre en **UTF-16** (`0x55920C` es el ANSI
  `".\Data\OTOMEDIUS_OP_f.wmv"`, convertido con `MultiByteToWideChar` en
  `0x4174B0`); se lee a `g_movie_path` y se convierte con `win_host_path()` a
  `g_movie_host`.
* `build_video_mediatype()` ofrece **RGB24** (no RGB32/YUY2), 1280x1024,
  `biBitCount=24`, `lSampleSize=1280*1024*3`, `AvgTimePerFrame=333667` (30 Hz).
  Antes se ofrecía RGB32 y el renderer lo rechazaba con `VFW_E_TYPE_NOT_ACCEPTED`
  (`0x8004022A`).
* `gb_Render`: los `HRESULT` se comparan como **`(int32_t)`**; antes `r >= 0`
  sobre `uint32_t` daba por bueno el intento fallido.
* `fg_AddFilter`: si `g_state == 0` (grafo nuevo) se limpia `g_nfilters`; el
  filtro del renderer de la sonda de arranque ya estaba liberado y se usaba.
* `mc_Run`: arranca el decodificador **solo si el manager del vídeo es dueño de
  `0x5DE258`** (`[0x5DE258] != 0 && M32([0x5DE258]) == 0x55E904`); así la sonda
  `0x417480` no genera frame ni pitido. `mc_Stop` cierra el decodificador.
* `dshow_movie_tick()` (llamado en cada `Present`): toma
  `[0x5DCF88] -> [+0x10] -> [+0x160]` (la textura del renderer) y copia el
  frame más reciente con `movie_frame()`/`texture_argb()`.

`native/movie.c` / `movie.h` (nuevo):

* Lanza `ffmpeg -v error -nostdin -re -i <wmv> -f rawvideo -pix_fmt bgra -`
  (fork/exec, pipe) y un hilo lector con anillo de 3 frames.
* `movie_frame(dst, dst_w, dst_h)` escala con vecino más cercano y tiene el
  camino rápido 640x512 -> 1280x1024 (2x) y fuerza alfa 0xFF.
* Audio: segundo `ffmpeg -vn -f s16le -ar 48000 -ac 2 -` y otro hilo que llama a
  `platform_audio_write()`.
* Cierre: `SIGKILL` al proceso, `join` de los hilos, `waitpid`, `close`.

El cuadro del vídeo **del propio juego** (`0x43F540`) sale degenerado en el
port: vértices `(1,inf)-(2561,1024)` (un `fdiv` a cero en su secuencia x87,
constantes `0x577528=1024.0`, `0x577530=1280.0`, `0x5773E0=2^32`). Por eso
`render.c` **compone** la textura del vídeo en el framebuffer software
(`movie_draw_tick`), aplicando el fade del manager (`[0x5DE224]`, solo en los
estados 2 y 4) y con `memcpy` cuando el tamaño coincide y no hay fade.

### 7.3 Audio (`platform.h`, `platform_sdl.c`, `tests/platform_null.c`)

* API nueva: `platform_audio_open(rate, channels)`, `platform_audio_write`,
  `platform_audio_close`.
* SDL3: `SDL_InitSubSystem(SDL_INIT_AUDIO)` +
  `SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
  {S16LE, 2, 48000})`, `SDL_PutAudioStreamData`.
* La espera de cola (~200 ms) es **acotada** (máx. ~500 ms) y descarta el trozo
  si el dispositivo se para: evita bloquear el cierre.
* El backend headless acepta las llamadas y las descarta.

### 7.4 Log UTF-8

`win32_misc.c` exporta `win_sjis_to_utf8()` (iconv CP932 -> UTF-8) y `font.c`
la usa para el nombre de la fuente: `font: CreateFontA(h=48
face=ＤＦＧ中太丸ゴシック体)`.

### 7.5 Cierre seguro con Esc a mitad de vídeo

Dos fallos corregidos:

1. **Cuelgue**: `platform_audio_write` esperaba sin límite a que bajara la cola;
   con el dispositivo parado el hilo de audio no terminaba y `movie_close()`
   bloqueaba el hilo del juego (GNOME "no responde"). Ahora la espera es
   acotada; además `platform_sdl.c` llama a `movie_close()` al recibir
   `Esc`/`SDL_EVENT_QUIT`, antes de `win_request_quit()`.
2. **`call` a NULL**: el apagado (`0x4050B0`, llamado solo desde `0x402238`)
   libera los objetos en `0x5E1F88`/`0x5E1F8C`; con el vídeo interrumpido uno
   llegaba con la **vtable a cero** y el juego hacía `call [0+8]` ->
   `x86: no recompiled code at 00000000`. `patches.c` reimplementa `0x4050B0`
   con el mismo cierre pero tolerando el objeto muerto (log
   `config: sound object ... has no vtable`).

Verificado: `quit@60`, `quit@300`, `quit@1200` (con `OTOMEDIUS_QUIT_AT`) salen
con `exit=0` y sin crash, con y sin vídeo.

### 7.6 Qué falta (medido en esta sesión)

* **Boot/attract (`0x4547E0`)**: sigue en `state=1 sub=8` con
  `slot258=0`, `mask220=0`, `mask224=0xFFFFFFFF`. Datos nuevos:
  - tabla de estados `0x45598C` = `{0:0x454893, 1:0x454B51, 2:0x454BE4,
    3:0x454D95, 4:0x454E43, 5:0x454F1C}`;
  - tabla de sub-estados `0x4559A8` (16 entradas, `[8]=0x454A74`);
  - **el dispatch de sub-estados (`0x454950`) está dentro del handler del
    estado 0**, y el handler del estado 1 (`0x454B51`) solo compara `sub` con 0
    y 1: **`state=1 sub=8` es un estado muerto** (no ejecuta nada);
  - el estado 0 sub 6 (`0x454A29`) crea el manager del vídeo: alloc `0xC` con
    `0x4142B0(0x5E1F28, 0xC)` + ctor `0x43F280` en `0x454A4A`;
  - el estado 0 sub 8 (`0x454A74`) crea otro manager: alloc `0xC` +
    ctor `0x429210` en `0x454A95` (vtable `0x55AE88`, update `0x429340`,
    draw `0x4294C0`), también en el slot `0x5DE258`.
* **Texto**: sigue pendiente `GGO_GRAY8_BITMAP`/`GGO_BITMAP` (§5.2).
* **Audio del juego**: `dsound` sigue mudo; solo suena el vídeo.
* **Quad del vídeo del juego**: `0x43F540` degenerado (arriba).

### 7.7 Comandos de verificación usados

```sh
# build local (headless) y SDL
sh /tmp/opencode/build.sh <ficheros.c...>
# regenerar (84 ficheros con --chunk 60)
PYTHONPATH=../.work/pylib python3 tools/recomp/recomp.py --exe ../.work/oto/otomedius.exe --out ../.work/gen --chunk 60
sh /tmp/opencode/gen_build.sh

# bezel + fullscreen (OpenGL, sin ventana)
cd ../.work/oto
SDL_VIDEODRIVER=offscreen OTOMEDIUS_BEZEL_DUMP=/tmp/opencode/w.ppm OTOMEDIUS_BEZEL_DUMP_AT=120   /tmp/opencode/otomedius-sdl .

# vídeo + audio + dump de un frame
LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu/codecs-extra/lib   OTOMEDIUS_MOVIE=1 OTOMEDIUS_DUMP_FB=/tmp/opencode/v.ppm OTOMEDIUS_DUMP_AT=600   ../obj/otomedius-recomp .

# cierre limpio a mitad de vídeo
OTOMEDIUS_QUIT_AT=300 OTOMEDIUS_MOVIE=1 ../obj/otomedius-recomp .
```

### 7.8 Ficheros de esta sesión

```
M native/platform_sdl.c   bezel, fullscreen/F11, audio SDL, movie_close en Esc
M native/platform.h       platform_audio_open/write/close
M native/dshow.c          RGB24, CoTaskMemAlloc, grafo nuevo, decoder, tick
M native/render.c         OTOMEDIUS_MOVIE (manager 0x43F280) + composición
A native/movie.c/.h       decoder vídeo+audio con ffmpeg
M native/win32_misc.c     CoTaskMemAlloc real; win_sjis_to_utf8 exportada
M native/win32.h          declaración de win_sjis_to_utf8
M native/d3d9_objects.c   GetLevelDesc (D3DSURFACE_DESC) + log MOVIE_DBG
M native/font.c           nombre de fuente CP932 -> UTF-8 en el log
M native/patches.c        OTOMEDIUS_BOOTDBG + 0x4050B0 tolerante
M tests/platform_null.c   stubs de audio
M flatpak/..-launch       copia bezel.png; LD_LIBRARY_PATH codecs-extra
M README.md, docs/HANDOVER.md
```

---

## 8. Sesión 2026-11-16 (segunda parte): boot completo, texto real y rasterizador

### 8.1 Boot: por qué se quedaba en `state=1 sub=8`

La causa era **el propio workaround de §4.16**, no el juego. `patches.c` forzaba
`[mgr+0xC] = 1` (state 0 → 1) y limpiaba `0x5DE258` en `state 1 / sub 1`. La
secuencia real (tabla de estados `0x45598C`, tabla de sub-estados `0x4559A8`
con 12 entradas) es:

```
state 0 (0x454893)  sub 0 0x454957  bms_eventlog_request_otdemo + bookkeep +
                                   alloc 0xC + ctor 0x4399A0 (KonamiLogo)
                    sub 1 0x4549BD  espera slot258 == 0
                    sub 2 0x4549CF  alloc 0xC + ctor 0x429860 (eAMUSELogo)
                    sub 3 0x4549BD  espera
                    sub 4 0x4549FA  alloc 0xC + ctor 0x45FE40 (Warning)
                    sub 5 0x4549BD  espera
                    sub 6 0x454A29  alloc 0xC + ctor 0x43F280 (vídeo)
                    sub 7 0x454A58  slot258==0 -> state=1, sub=0, io=0
state 1 (0x454B51)  sub 0 0x454B8C  alloc 0x2C + ctor 0x45EA30 (Title)
                    sub 1 0x454B62  si slot258==0:
                                    [0x5E5F0C]==0 -> sub=8 y salta a 0x454DC0
                                        (fade + state=0, sub=0)
                                    [0x5E5F0C]!=0 -> state=2, sub=0
state 2 (0x454BE4)  card entry / eAMUSE / network (sub 0..5)
state 3 (0x454D95)  warning -> 0x454DC0 -> state=0 sub=0
state 4 (0x454E43)  resultado de card entry -> state 5 o 6
state 5 (0x454F1C)  boot del juego / juego; puede ir a state=3 (warning)
state 6 (0x4553C4)
```

El dispatch de sub-estados (`0x454950`, `jmp [edx*4+0x4559A8]`) se ejecuta
**siempre** que `slot258 != 0`, o cuando `[0x5E2136] != 1` (ruta `0x45493B`).
El parche forzaba `state=1` en la entrada, saltándose el dispatch del estado 0;
luego `state 1 sub 1` ponía `sub=8` y saltaba a `0x454DC0` (que pone
`state=0`), y en el frame siguiente el parche volvía a forzar `state=1` con
`sub=8` → **estado muerto**. Solución: eliminar todos los forzados; el estado
0 crea KonamiLogo/eAMUSELogo/Warning/vídeo, el 7 pasa a Title, y luego a
`state=0 sub=8` (manager `0x429210`) y `sub=9..11` (escena/attract).

### 8.2 Loader: tabla de 16 paquetes y commit del id 9

`0x418040` recibe `slot[+4]` como **id de asset/paquete**, no como índice de
una tabla de 4. `0x4186F0` usa `[esi*4 + 0x5DE098]`; la tabla tiene **16
entradas** (igual que los 16 slots del ring `0x5DCF98`), y los logos piden el
id **9**. El guard anterior (`id >= 4 → skip`) tiraba todos los jobs de logo:
por eso `KonamiLogo.pak` nunca se abría. Corregido a `id >= 16`.

`0x418190` (commit/limpieza del paquete) recorre `vector1` (`pkg+4/+8`), y por
cada grupo `[grupo]=count` con elementos de **0x44 bytes** (`+0` puntero a
objeto, `+4` nombre de 0x40) llama `[[obj]+8]` (destructor) y libera. Con
`Title.pak` hay un record con objeto/vtable NULL → `call [[0]+8]` =
`no recompiled code at 00000000`. Añadido un escaneo previo en `0x418190` que
salta el commit sólo si encuentra un record muerto. Para el **paquete 9**
(assets compartidos de boot/attract: KonamiLogo, eAMUSELogo, Warning, Title)
el commit se **difiere**: se saltan siempre los commits del id 9 y en la
entrada de `0x4186F0` se hace `pkg+8 = pkg+4` (descarta el grupo anterior; el
bloque se deja leakado) **sólo si el stream no es un banco de sonido
`"SDBK"`**. Esto evita el crash y conserva el grupo para el draw.

### 8.3 Texto: `GGO_GRAY8_BITMAP` + DDS de fuente + bug del rasterizador

Lo implementado en `font.c`:

* `GGO_GRAY8_BITMAP` (6): `FT_Load_Glyph(FT_LOAD_RENDER|FT_LOAD_NO_BITMAP|
  FT_LOAD_TARGET_NORMAL)`, `stride=(w+3)&~3`, un byte/píxel, cobertura 0..255
  escalada a **0..64** (`(cov*64+127)/255`). Es obligatorio: `CTexFont+0x58`
  es una paleta de 256 entradas que sólo define bien 0..64 (medido:
  `pal[0]=0, pal[1]=3, pal[16]=63, pal[32]=127, pal[48]=191, pal[63]=251,
  pal[64]=255, pal[65]=255, pal[80]=184, pal[128]=0`). Con 0..255 el atlas
  salía como ruido.
* `GGO_BITMAP` (1): 1bpp MSB-first, `stride=((w+31)/32)*4`.
* `GGO_NATIVE` (2) sólo para `format == 2` (antes `format & 2` capturaba el 6).
* `GLYPHMETRICS`: `gmBlackBoxX/Y`, `gmptGlyphOrigin`, `gmCellIncX/Y` y
  **`gm+0x10` = avance** (lo lee `0x409F5E`; el gm local tiene 20 bytes).

La causa de que el texto siguiera roto no era el atlas: **`draw_tri()` en
`render.c` hacía `area = fabs(area)`**. Los quads del juego llegan en sentido
horario (área negativa), así que los pesos baricéntricos sumaban −1 y las UV
salían negadas; con WRAP eso muestrea la celda opuesta (texto roto) y gira
180° cualquier textura (el logo de eAMUSEMENT). Conservar el signo del área
arregla **las dos cosas**. Esto afecta a todo el pipeline 2D texturizado.

Las dos texturas de fuente del juego no son el atlas FreeType: `0x401000`
llama a `D3DXCreateTextureFromFileExA` para `Data/SysFont.dds` (128×128,
16bpp **A4R4G4B4**, 8×8 por glifo) y `Data/gradius_font.dds` (256×256,
**DXT5**, celdas 24×24, `cu=cv=0.09375=24/256`). El stub devolvía `S_OK` con
`ppTexture = NULL` → `[0x5E1F64]/[0x5E1F68] = 0` → `0x5BE7CC/0x5BE7E8` con
`tex=0` → texto sin textura. `d3dx_CreateTextureFromFileExA` ahora lee el
fichero con `win_host_path()`, parsea DDS y crea/sube la textura.
`decode_dds` entiende DXT1/3/5, A8R8G8B8, **A4R4G4B4** (f00/f0/f/f000),
**A1R5G5B5/R5G6B5** (f800/7e0/1f) y `texture_argb` los decodifica.
`D3DXGetImageInfoFromFileInMemory` sigue devolviendo w/h + 32 en `+8`; no se
usa el `Format` para nada crítico.

### 8.4 Vídeo: el `inf` de 0x43F540 y el temporizador de frames

La secuencia x87 de `0x43F540` es realmente:

```
[esp+0xc]=w(frame) [esp+0x10]=h [esp+0x14]=tex_w [esp+0x18]=tex_h
fld tex_h; fld st0; fdivr 1280      -> tex_h/...
fld h;     fld st0; fdivr 1024
fld tex_w; fld st0; fldz; fld st0
fdivrp st(2)                        -> 0/tex_w = 0
...
fld tex_h; fld st0; fdivr st(2)     -> tex_h / 0 = +inf
```

El `+inf` entra en la coordenada Y del quad (se veía `(1,inf)-(2561,144)`).
`patches.c` intercepta `0x41C620` (setup 2D) y convierte a 0 cualquier
coordenada no finita del struct `[eax+0x10..0x1C]`. El draw nativo queda
`(1,0)-(2561,1024)` (la X sigue duplicada por el escalado que la pila x87 deja
para `0x41C500`), y se puede ver con `OTOMEDIUS_NATIVE_MOVIE=1`. Por defecto
sigue la composición de `render.c` (`movie_draw_tick`), que es la que da el
vídeo correcto.

El manager de vídeo (`0x43F370`) cuenta 0xDF2 = 3570 *frames* (59.5 s a 60 Hz).
El port va a ~34 fps (render software + mixer de sonido del juego), así que el
intro tardaba ~107 s y el último frame quedaba congelado ~47 s. `patches.c`
intercepta `0x43F370`: en `state 3` añade los ticks de 60 Hz transcurridos
(`clock_gettime(CLOCK_MONOTONIC)`, redondeo) con clamp a `0xDF1` para que el
`+1` del juego y su `cmp 0xDF2` sigan disparando. Ahora el intro dura ~60 s.

### 8.5 Sonido: `DuplicateSoundBuffer` y TEB

El motor de sonido duplica el buffer de una onda por voz (`0x41BE20` →
`IDirectSound8::DuplicateSoundBuffer`, vtbl+0x14) y espera el duplicado en el
3er argumento. Devolver `E_NOTIMPL` dejaba `[voz+8]=NULL` pero la voz se
encolaba igualmente, y el mixer llamaba `[[voz+8]]+0x40` (vtable NULL) →
`0x41BFB8`. Implementado `dsbuffer_clone()` (comparte los samples, sin
realloc), y `b_QueryInterface` devuelve `this` para IUnknown/
IDirectSoundBuffer/IDirectSoundBuffer8 y `E_NOINTERFACE` para
IDirectSoundNotify. El audio del juego sigue mudo (no hay mixer dsound→SDL).

El hilo de audio (`0x41B2A0`) llamaba `TlsGetValue` con TEB inválido y
petaba en `x86_tls_get`. Añadido `teb_ok()` (rango 0x7FF00000..0x7FFFFFFF) y
el fallo imprime `x86: TlsGetValue with invalid TEB ...` y devuelve 0.
También se añadió `backtrace()` + RIP/RSP del `ucontext` al `fault_handler`.

### 8.6 Estado verificado / pendientes

Verificado en headless con `OTOMEDIUS_LOG=1 OTOMEDIUS_BOOTDBG=1`:

* state 0 sub 0→7 (KonamiLogo, eAMUSELogo, Warning, vídeo), `state=1 sub=0`
  sobre el update #2458, `Title.pak`/`TitleSnd.pak` abiertos, y `state=0
  sub=8/9` (attract) cargando `Speed.pak`, `Player01.pak`, `Player02.pak`.
  Sin `no recompiled code` en el tramo de logos/vídeo/Title.
* `NOW INITIALIZING` y el aviso de “operating in local mode / eAMUSEMENT
  cannot be used” se dibujan legibles con la fuente real.
* eAMUSEMENT upright (el DDS `e_logo.dds`, 1024×1024 DXT1, es correcto).
* Esc a mitad de vídeo / `OTOMEDIUS_QUIT_AT=900` → `exit=0`.
* `OTOMEDIUS_NO_FREETYPE=1` sigue funcionando (fallback sintético).

Pendiente:

1. **KonamiLogo sigue sin verse en el flujo normal.** Su `konami_logo.dds`
   (1024×256, **A1R5G5B5**) sí carga si se quita `KonamiLogoSnd.pak`:
   verificado `pkg9=10069598 vec=169ccf60..169ccf64 rec=100695f8 n=1
   tex=60930e20`. Con los dos paks, el vector queda `vec=0`: el job de sonido
   (mismo id 9, stream `SDBK`/`WVBK`) corre concurrente con el del logo y el
   grupo no llega a añadirse (posible mezcla del estado global de stream
   `0x5DCF84` o del ring). Los dos `KonamiLogo*.pak` se abren, pero el grupo
   no persiste. Próximo paso: serializar los jobs por id de paquete (el
   juego ya intenta un solo worker vía `0x419170` + `WaitForSingleObject(h,0)`;
   comprobar por qué llegan a arrancar dos `0x419D00` a la vez) **o** capturar
   el grupo del logo en `0x4186F0`/`0x41DDA0` antes de que el job de sonido
   lo pise.
2. `Player02.pak` (attract) provoca `kernel32: unhandled exception` (SEH) y el
   port sale con `exit=0`. Es el siguiente fallo del loader, no del boot.
3. Draw nativo del vídeo: la X sigue duplicada por el escalado x87 que
   `0x41C500` toma de la pila; la composición es el camino por defecto.
4. El audio del juego (dsound) sigue mudo.

### 8.7 Ficheros de esta sesión

```
M native/font.c/.h       GGO_GRAY8_BITMAP, GGO_BITMAP, avance en gm+0x10
M native/win32_misc.c    ruteo por formato exacto + fallback gray8
M native/d3dx9.c         D3DXCreateTextureFromFileExA real; DDS 16bpp
M native/d3d9_objects.c  TF_A4R4G4B4, TF_A1R5G5B5, TF_R5G6B5
M native/render.c        draw_tri: conservar el signo del área (fix 180°)
                         OTOMEDIUS_NATIVE_MOVIE
M native/dsound.c        DuplicateSoundBuffer (clone), b_QueryInterface
M native/patches.c       boot sin forzados; id<16; commit id9 diferido;
                         guard de records muertos; clamp ±inf en 0x41C620;
                         temporizador real del vídeo; logs BOOTDBG
M native/main.c          TEB check en TLS; backtrace + RIP/RSP al fallar
M README.md, docs/HANDOVER.md
```

---

## 9. Sesión 2026-11-16 (tercera parte): loader sin carreras, logo de Konami, thunks, rasterizador

Esta sección **manda** sobre §3–§8 donde haya contradicción.  Tiene la misma
estructura de seis puntos que el documento original.

### 9.1 Entorno, toolchain y herramientas (delta)

* Scripts persistentes en **`.work/tools/`** (`/tmp/opencode` se vacía):
  `sh .work/tools/build.sh <archivo.c...>` (zig cc `-O2 -std=gnu11 -w`,
  recompila los `.c` nativos indicados y re-linkea contra `.work/obj/*.o` +
  `.work/obj/gen/*.o`; `tests/platform_null.c` se resuelve por nombre),
  `python3 .work/tools/xd.py <hex addr> [n]` (desensamblador capstone sobre el
  `otomedius.exe` original), `python3 .work/tools/ppm2png.py in.ppm out.png [escala]`.
* **Ghidra MCP** operativo: proyecto `gghidra`, programa `/otomedius.exe`
  (4533 funciones).  Desde `execute`: `tools["ghidra-mcp"].force_decompile({address:"0x419430"})`.
  También por HTTP: `curl 'http://127.0.0.1:8089/force_decompile?address=0x419430'`.
* Corridas largas (Title ≈ present 2750, attract ≈ 4500) tardan 100–200 s:
  lanzarlas en background con `timeout`, escribir el log a fichero y volcar
  frames con `OTOMEDIUS_DUMP_AT=… OTOMEDIUS_DUMP_EVERY=…`.  Un `[dump] N
  dispatches` doble al final es el handler de SIGTERM del `timeout`, no un crash.
* Identidad git del repo: `nicobm <215758560+nicobm@users.noreply.github.com>`,
  remoto `https://github.com/nicobm/otomedius-recomp.git`, rama `master`.
  **No hay `gh` ni credential helper**; el push se hizo con un token pasado por
  variable de entorno de un único comando.  **Nunca** escribir el token en el repo.

### 9.2 Mapa de memoria y globals (delta)

Cola/loader asíncrono (todo en `.data` no respaldado por archivo, §2.2):

| Dirección | Significado |
|---|---|
| `0x5BC914` | handle del hilo worker (`0` = ninguno) |
| `0x5BC918` | id del job que el worker está procesando |
| `0x5BC91C` | estado: `0` idle, `1` corriendo, `2` terminado, **`3` CANCELADO**, `4` error |
| `0x5DCF8C` | id "actual" que consulta el pump |
| `0x5DCF90` / `0x5DCF94` | head / tail del ring (16 slots × `0x10C` en `0x5DCF98`) |
| slot `+0` / `+4` / `+8` | id / id (se pone a `-1` al cancelar o liberar) / nombre del pak (`0x104` bytes) |
| `0x5DC9A2` | loader activo |
| `0x5DE070` | `CRITICAL_SECTION` del loader |
| `0x5DE098 + id*4` | tabla de **16** paquetes; `0x5DE0BC` = id 9 (logos/warning/title/attract) |
| `pkg+4 / pkg+8` | vector de grupos (begin/end, punteros a grupo) |
| grupo | `[0]=count`, luego `count` entradas de `0x44`: `+0` puntero a textura, `+4` nombre (`0x40`) |
| `0x5DB708` | cabecera de `testmode.bin`: magic `0x07091300`; **`0x5DB710` = offset de reloj s64** (en este dump `0xFFFFFFFFFFEC29E0` = −1 300 000 ms) |

Otros: `0x5E1F20` device D3D9; `0x5DE258` slot del manager "exclusivo" (logo/warning/vídeo/attract);
`0x5DE23C` manager KonamiLogo; `0x5E1F7C` log object.  Thunks nativos:
`THUNK_BASE=0x7F000000`, paso 16, **tabla fija de `0x10000` entradas de 24 B**
(`native/imports.c`).

### 9.3 Funciones (delta) — loader y logo

| Dirección | Rol (verificado con Ghidra salvo nota) |
|---|---|
| `0x419170` | **Enqueue**: `EBX`=id del paquete, arg0=nombre; escribe slot[tail], `WaitForSingleObject([0x5BC914],0)`: si devuelve 0 (hilo terminado) cierra el handle y llama `0x419CA0`; si no, reutiliza el worker; avanza `tail` |
| `0x419330` / `0x4193A0` | `Cancel(id)` en `ESI` (§3.3) |
| `0x419430` | **pump** (main thread): devuelve `1` si el id actual está encolado, `2` si no es el job actual, o el estado; lanza el worker si head≠tail; `4` si falla `_beginthreadex` ("2303:Failed Begin Thread") |
| `0x419530` | `Sleep(16)` + pump; devuelve `1` si pump∈{0,2} |
| `0x419CA0` / `0x419D00` | crea / cuerpo del worker (`Once.pak` y compañía) |
| `0x418040(id,name)` | ejecutor del job: `0x40EE90` (abre y mapea el pak) → `0x4185B0` (descomprime CAB) → `0x4186F0` (construye el grupo; llama `0x41DDA0`); errores 2300/2305/2306 |
| `0x41DDA0` | **constructor de grupo**: `0x4142B0` aloja `(n-1)*0x44+0x48`, y por entrada `strcpy_s(nombre)` + `D3DXGetImageInfoFromFileInMemory` + `D3DXCreateTextureFromFileInMemoryEx(dev,…, mip=1, fmt=0xFFFFFFFD?, pool=1, …)`; errores "2500 Failed Alloc Group" / "2501 Failed Create Tex:%s" |
| `0x418190(id)` | commit/limpieza del paquete (§4.13, §8.2) |
| `0x4399A0` | ctor del manager KonamiLogo (vtable `0x55C8A0 = {0x4399F0, 0x44E630, 0x439A70, 0x439B90, 0x41DF40}`, registra en `0x5DE258`, `[obj+4]=0x1000`) |
| `0x439A70` | update del logo: estado en `[obj+0xA]` (0..4, tabla `0x439B7C`); estado 0 pide `".\Data\KonamiLogo.pak"` (`0x55C884`, id 9) |
| `0x439B90` → `0x439BB0` | draw del logo: textura = `grupo[0]+0` de `pkg9` (`[0x5DE0BC]`); sin paquete dibuja un quad blanco |
| `0x439A10` | dtor del manager; llama `0x418190(9)` |
| `0x40CAF0` | reloj→cadena (`ctime_s` vía `0x52F26E` → helper time64 `0x52DF0D`); usa `0x40CAD0 = 0x40CA10() + [0x5DB710]` |
| `0x52DF0D` / `0x52FC65` | helper CRT time64: rechaza negativos o > año 3000 con `EINVAL (0x16)` → *invalid parameter handler* → `UnhandledExceptionFilter` |

### 9.4 Parches y arreglos (todo sin commit antes de esta sesión)

1. **`k_CreateThread` (`win32_kernel32.c`)**: `thread_arg_t` pasa de `malloc` a
   **`calloc`**.  `done` basura (`300`, `555`, `1677721700`) hacía que
   `WaitForSingleObject(h,0)` dijera "terminado" y `0x419170` arrancara un
   segundo `0x419D00` concurrente.  Era la causa del Bloqueo 1 (vector del
   paquete vacío) y del mito "el handshake nunca completa".
2. **Guard del loader (`patches.c`, `0x419530`)**: ahora **opt-in**
   (`OTOMEDIUS_LOADER_GUARD=1`).  Antes forzaba "terminado" a los 52 polls
   mientras el worker aún descomprimía `Once.pak` (~240 presents), y el job de
   `KonamiLogo.pak` se ejecutaba tras terminar la etapa del logo.
3. **`bms_time_get_clock_blocking` (`avs.c`)**: `CLOCK_REALTIME` en ms.  El
   offset de `testmode.bin` es negativo; con reloj monotónico la suma daba un
   `time64` negativo, `ctime_s` fallaba y el juego moría con *unhandled
   exception* antes del logo (`0x52DF9B`).
4. **Tabla de thunks (`imports.c`)**: array estático `g_thunks[0x10000]` +
   mutex en `x86_native_register`.  El `realloc` anterior corrompía lecturas
   del hilo del mezclador (`fn_0041b2a0` → `x86_native_call`) → SIGSEGV con
   dirección de heap del host (`0x…8248`) al cargar `Stage01.pak`.
5. **`dsbuffer_clone` (`dsound.c`)**: el clon reutiliza la vtable del original
   (antes registraba un juego nuevo de thunks por voz).
6. **`UnhandledExceptionFilter` (`win32_kernel32.c`)** imprime código,
   dirección, contexto y pila; **`fault_handler` (`main.c`)** imprime la última
   importación nativa del hilo.
7. **Diagnóstico**: `OTOMEDIUS_DUMP_EVERY`, `OTOMEDIUS_DRAWDBG`,
   `OTOMEDIUS_TEXFMT`, trazas `bootdbg`/`LOADER` con contador de presents
   (`render_presents()`), stubs de `UpdateSurface/StretchRect/ColorFill/
   DrawIndexedPrimitiveUP/ProcessVertices/DrawRectPatch/DrawTriPatch/
   SetScissorRect` que avisan una vez si se llaman (en la corrida al Title
   **no se llamó ninguno**).
8. **Rasterizador (`render.c`, `d3d9.c`)**: ahora usa el **FVF real**
   (`g_fvf`; antes asumía `0x144` y `DrawPrimitiveUP` pasaba `arg32(5)` basura
   como FVF); estados `RS_ALPHABLENDENABLE/SRCBLEND/DESTBLEND/ALPHATEST/ALPHAREF/
   ALPHAFUNC/TEXTUREFACTOR`; operaciones del stage 0 (`COLOROP/ARG1/ARG2`,
   `ALPHAOP/ARG1/ARG2` con SELECTARG1/2, MODULATE(2X/4X), ADD, SUBTRACT,
   TFACTOR, complemento/replicado de alfa); sampler stage 0 (WRAP/MIRROR/CLAMP,
   filtro POINT/LINEAR bilineal); color de vértice interpolado.  Sin textura
   se usa el difuso tal cual.  `render_set_texture_stage_state()` es nuevo y se
   llama desde `dev_SetTextureStageState`.  El truco de UV "por celdas" del
   texto se conserva.

### 9.5 Estado actual y bloqueos

**Verificado (headless, ~200 s):** Konami logo visible (textura 1024×256
`A1R5G5B5` en `(128,384)-(1152,…)`; el logo real tiene los trazos de velocidad
sobre "KO"), eAMUSELogo, Warning, vídeo (~60 s), Title (`Title.pak`,
`TitleSnd.pak`, `title_stream.xwb`), attract (`replay000.pak`,
`catch_demo02.pak`, `Player02.pak`, `Speed.pak`, `TwinMsil`, `CycLaser`,
`Ripple`, `MultiOpt`, `Bomb`, `Stage01.pak`), sin `no recompiled code`, sin
SEH, sin SIGSEGV.

**Bloqueo activo — fondo del Title.**  Present 3400 (33 draws `DrawPrimitiveUP`):
28 quads de texto (`tex 256×256`), un quad `fvf=44` blanco a pantalla completa,
un quad `fvf=144` con `tex=…9500 1024×1024` en `(128,0)-(1152,…)`, color
`0x7FFFFFFF` (alfa 50 %), un quad `tex 2048×512` en `(40,362)` con UV `0..0.586`,
color `0x5BFFFFFF`, y el overlay rojo/negro de la consola de depuración.  El
logo "オトメディウス" sale bien, pero el **fondo se ve como puntos sueltos**
(filas de píxeles aislados) en vez de una imagen continua.
Hipótesis, por orden: (a) `decode_dds`/`texture_upload` decodifican mal el
formato de esa textura (probar `OTOMEDIUS_TEXFMT=1`; formatos no soportados caen
a ARGB; DXT3 vs DXT5, `A8L8`, `L16`, `A8`…); (b) la caché de 64 entradas de
`texture_argb()` entrega un buffer parcial para texturas grandes; (c) el
`D3DXCreateTextureFromFileInMemoryEx` del grupo (`0x41DDA0`) ignora `Format`/
`MipLevels`/`Pool` pedidos (usa siempre A8R8G8B8 1 nivel).
`OTOMEDIUS_TEXFMT` ya está implementado en `d3dx9.c` y compila; **todavía no se
ha ejecutado**.

Otros pendientes: attract casi en blanco (present ≥ 5000: barra gris arriba,
caja negra con texto minúsculo al centro); en la pantalla de aviso de eAMUSEMENT
la parte izquierda del texto sale con basura amarilla y hay un triángulo negro
suelto arriba a la derecha (observado antes del rewrite del rasterizador, no
reverificado); audio de `dsound` mudo (no hay mezclador `dsound`→SDL); quad
nativo del vídeo (`0x43F540`) con X duplicada; rendimiento ~26–36 fps
(rasterizador por píxel, flotante).

### 9.6 Plan de acción inmediato

1. `sh .work/tools/build.sh d3dx9.c`; correr en background
   `OTOMEDIUS_TEXFMT=1 OTOMEDIUS_DRAWDBG=3400 OTOMEDIUS_DUMP_FB=/tmp/opencode/t
   OTOMEDIUS_DUMP_AT=3400 timeout 170 ../obj/otomedius-recomp .` y leer el formato/
   tamaño de la textura 1024×1024 (y la de 2048×512).  Corregir `decode_dds`/
   `texture_upload` (y `texture_argb`) según el formato real.  Verificar con un
   dump que el fondo es continuo.
2. Repetir `DRAWDBG`+dump en presents ≈ 4600, 5000 y 6500 (attract): comparar
   los draws con lo que el juego debería mostrar; buscar texturas con `tex=0`,
   alfa 0 o FVF no soportados (`!(fvf&4)` se descarta en silencio en
   `draw_up_vertices`); revisar `DrawPrimitive`/indexados (`draw=0` siempre).
3. Mezclador de audio: implementar `dsound`→SDL (`platform_audio_*` ya existe,
   §7.3) leyendo los buffers `dsbuffer_t` (`Play/Stop/Lock/SetVolume`, clones
   comparten `data`), y revisar el hilo `0x41B2A0`.  Después: draw nativo del
   vídeo y rendimiento del rasterizador.

### 9.7 Prompt para la sesión nueva

```
Repo: /var/home/vm/Proyectos/SALA DE TRABAJO/otomedius-recomp (recompilación
estática de Otomedius, Flatpak + SDL3). Lee docs/HANDOVER.md entero; manda §9.
No hagas commits ni push sin pedirlo; nunca escribas tokens en archivos.
Herramientas persistentes: .work/tools/{build.sh,xd.py,ppm2png.py}
(sh .work/tools/build.sh <archivo.c>; gen: PYTHONPATH=../.work/pylib python3
tools/recomp/recomp.py --exe ../.work/oto/otomedius.exe --out ../.work/gen --chunk 60;
run: cd ../.work/oto && ../obj/otomedius-recomp .). Ghidra MCP activo con
/otomedius.exe (tools["ghidra-mcp"].force_decompile({address})).

Estado: boot completo (KonamiLogo visible -> eAMUSELogo -> Warning -> vídeo ->
Title -> attract hasta Stage01.pak) sin crash. Arreglado: thread_arg_t.done sin
inicializar (calloc), guard del loader opt-in, reloj bms CLOCK_REALTIME,
tabla de thunks fija, clones dsound con vtable compartida, rasterizador con
FVF/blend/alphatest/TSS/sampler.
BLOQUEO: fondo del Title (quad 1024x1024 alfa 0x7F y 2048x512) sale como puntos
sueltos: probable decode DDS/texture_upload. OTOMEDIUS_TEXFMT=1 ya existe en
d3dx9.c, falta correrlo (background, ~150 s, present ~3400, con
OTOMEDIUS_DRAWDBG=3400 y OTOMEDIUS_DUMP_FB/DUMP_AT). Luego attract casi en
blanco, audio dsound mudo, quad nativo del vídeo.
```

---

## 10. Sesión 2026-11-16 (cuarta parte): formatos DDS reales, fondo del Title y mezclador dsound

Esta sección **manda** sobre §1–§9 donde haya contradicción.

### 10.1 Fondo del Title: el DDS es RGB24 (24 bpp), no A8R8G8B8

El quad de 1024×1024 con `tex=…9500` y alfa 50 % era `main_visual.dds`, y
el de 2048×512 era `t_logo.dds`. Cabeceras reales (extraídas de `Title.pak`
con `gcab`; el `.pak` es un CAB con un `.bin` que contiene nombre + DDS):

| Textura | Tamaño | `pf_flags` | `bits` | Máscaras | Formato |
|---|---|---|---|---|---|
| `main_visual.dds` | 1024×1024 | `0x40` | **24** | `ff0000/ff00/ff/0` | **R8G8B8** |
| `t_logo.dds` | 2048×512 | `0x41` | 32 | `ff0000/ff00/ff/ff000000` | A8R8G8B8 |
| `konami_logo.dds` | 1024×256 | `0x40` | 16 | `f800/7e0/1f/0` | **R5G6B5** (no A1R5G5B5) |
| `Player02*.dds` | 6 texturas | `0x41` | 16 | `7c00/3e0/1f/8000` | A1R5G5B5 |
| `catch_demo02` | 2048×1024 | `0x40` | **24** | `ff0000/ff00/ff/0` | **R8G8B8** |

`decode_dds()` sólo entendía DXT1/3/5 y los 16 bpp de fuente, y **todo lo
demás caía a `FMT_ARGB`**: un RGB24 leído como 32 bpp toma como alfa el canal
azul del píxel siguiente. Medido sobre `main_visual.dds`: el 12,4 % de los
píxeles quedaba con alfa < 128 → el fondo se dibujaba como puntos sueltos
(el resto, con colores desplazados). `catch_demo02.pak` (fondo del attract)
es también RGB24, así que el mismo bug afectaba al attract.

Arreglo (`native/d3dx9.c`, `native/d3d9_objects.c`):

* `decode_dds()` clasifica por `dwRGBBitCount` + máscaras, no por suposición:
  `R8G8B8` (24 bpp), `X8R8G8B8` (32 bpp sin `DDPF_ALPHAPIXELS`),
  `R5G6B5`, `A1R5G5B5` y `X1R5G5B5` (máscaras `7c00/3e0/1f`), además de los
  formatos ya existentes.
* Enums nuevos al final (`FMT_R8G8B8`, `FMT_X8R8G8B8`, `FMT_X1R5G5B5` y sus
  `TF_*` equivalentes) para no cambiar los valores ya usados.
* `texture_argb()` decodifica los tres: 24 bpp a `0xFFRRGGBB`; X8 fuerza alfa
  255; X1R5G5B5 no usa el bit 15 como alfa.
* **R5G6B5 tenía además el verde mal**: se decodificaba como 5-5-5
  (`r=(v>>10)&31`). Ahora es 5-6-5 (`r=(v>>11)&31`, `g=(v>>5)&63`).

Verificación: `OTOMEDIUS_TEXFMT=1` muestra
`texture 1024x1024 fmt=9 ... bits=24 masks=00ff0000/...` y el dump del
present 3400 (`/tmp/opencode/fix3400.png`) tiene el fondo continuo, con
`t_logo.dds` encima.

### 10.2 Mezclador `dsound` → SDL (`native/dsound.c`)

`0x41B2A0` **no mezcla PCM**: recorre 14 voces de `0x48` bytes en
`0x5DE0F0+4i`, y para cada una cuyo `[voz+8]` (buffer) está vivo y
`[voz+0x14]&9 == 0` consulta `GetCurrentPosition` y llama `Stop` cuando la
posición supera la longitud (`[voz+0x10]+0xC`). Es decir, el juego delega la
mezcla en DirectSound. El port ahora la hace:

* `dsbuffer_t` guarda `WAVEFORMATEX` (canales, rate, bits), `freq` de
  reproducción, `play_flags`, `is_primary`, `position` y una fase fraccionaria.
* Registro de objetos (4096) protegido por mutex; los clones comparten
  `data`/formato (siguen compartiendo la vtable).
* Hilo mezclador → `platform_audio_open(48000, 2)`; mezcla todas las voces
  con volumen en centibelios (`10^(v/2000)`), pan, 8/16 bits, mono/estéreo,
  resampleo por fase, `DSBPLAY_LOOPING`, y **avanza `position`** (sin eso el
  hilo del juego nunca para las voces; `GetCurrentPosition` la devuelve).
* `Lock` divide correctamente los bloqueos que dan la vuelta al buffer.
  `SetFormat`/`CreateSoundBuffer` leen el `WAVEFORMATEX`; `SetFrequency`
  cambia el rate de reproducción; `GetStatus` informa `PLAYING|LOOPING`.
* `platform_audio_*` usa un mutex (el mezclador y el decodificador del vídeo
  escriben el mismo stream SDL); `dsound_shutdown()` se llama antes de
  `platform_shutdown()`.
* Diagnóstico: `OTOMEDIUS_DSDBG=1` (CreateSoundBuffer/Play/Stop/SetVolume/
  SetFrequency/Lock) y `OTOMEDIUS_AUDIO_DUMP=file.raw` (S16LE 48 kHz estéreo).
  `OTOMEDIUS_AUDIO_STREAM_DUMP=file.raw` captura el **stream final** (vídeo +
  juego) tal como se envía a SDL.
* El mezclador y el decodificador del vídeo comparten el stream.  Para que la
  canción del vídeo no salga entrecortada, `platform_audio_write()` **suelta
  el mutex mientras espera cola** y el mezclador de dsound **no escribe**
  mientras `movie_active()` (sigue avanzando `position` para que el hilo del
  juego pare las voces).  Antes, el mezclador retenía el mutex durante la
  espera y ahogaba al vídeo.
* **Causa real del choppy**: el segundo ffmpeg (audio) llevaba `-re`, así que
  entregaba justo a tiempo real y la cola de SDL quedaba casi vacía; cualquier
  jitter del rasterizador producía underruns.  Ahora el proceso de **audio no
  usa `-re`** (decodifica por delante y llena la cola, que hace de colchón);
  el de vídeo sí lo conserva.  Verificado de oído por el usuario: la canción
  del intro suena limpia.

Medido en el boot/Title: el juego crea un buffer de 894 976 B (44,1 kHz
estéreo 16 bits), tres de 22 050 Hz mono 8 bits y uno de 352 800 B; pone
volúmenes de −36/−45 dB. El dump tiene ráfagas con pico ~380 y RMS ~33 (el
resto es silencio porque el propio juego no pide más sonido). El mezclador
entrega esos datos a SDL; el audio del juego ya no es "mudo por falta de
mezclador".

### 10.3 Ergonomía de pruebas

* `OTOMEDIUS_WINDOWED=1` + **`OTOMEDIUS_WINDOW_SIZE=WxH`** (por ejemplo
  `800x450`, 16:9) arranca en ventana pequeña. Fullscreen sigue siendo el
  default de producción; `F11` vuelve a la ventana configurada.
* **El ESC se registra**: `platform: Esc pressed` y, si `OTOMEDIUS_DUMP_FB`
  está definido, se escribe también `<DUMP_FB>.quit` con el frame exacto en
  que se pulsó (`dump_on_exit()`). El resto del log queda en el fichero de
  la corrida.
* Para corridas automáticas largas (llegar al present 5000+ sin que nadie
  cierre la ventana) usar `SDL_VIDEODRIVER=offscreen`; el modo windowed se
  reserva para comprobaciones visuales.

### 10.4 Attract: qué se ha verificado y qué queda

* Present 4600: el tutorial (`catch_demo02`, 2048×1024 RGB24) **ya se ve**
  (dump `att.04600` → `att4600.png`); antes era blanco porque era RGB24.
* Present 5000–6200: pantalla blanca estática con la consola de depuración
  (cuadro rojo/negro) y dos quads de texto reales; los otros 28 quads de
  texto van a `y=319040` (fuera de pantalla). El quad blanco a pantalla
  completa es `fvf=44` sin textura y alfa `0xFFFFFFFF`: es un **fade**, no
  una textura que falte.
* En los frames de 48 `DrawPrimitiveUP` sólo 35 llegan al rasterizador
  (`drawdbg`); los 13 restantes se investigan con los diagnósticos nuevos
  `SKIP(...)`/`DISCARD` (motivo, FVF, `v0`, y matrices WORLD/VIEW/PROJ).
  En el present 5000 exacto no aparece ninguno: pertenecen a la pantalla del
  tutorial, no a la blanca.  De hecho, con `OTOMEDIUS_DRAWDBG=1` en toda la
  corrida hasta 5203 **no hay ningún SKIP/DISCARD**: las 48 son calls de
  frames distintos (el contador de stats agrupa 120 presents), no draws
  perdidos ese frame.
* `draw=0` en todo el attract: el juego no usa `DrawPrimitive`/
  `DrawIndexedPrimitive`.
* El manager del attract es **`CDemoPlayMan` (`0x429210`, vtable
  `0x55AE88`)**: estado 0 encola `Data/replay000.pak` (id `[0x5E2024]`,
  rota 0..3), 1 espera `0x419530`, 2 crea `CEyeCatchMan` (`0x42CE60`, slot
  `0x5DE25C`), 3 espera a que termine, 4 crea `CGameMan` (`0x42E1F0`, slot
  `0x5DE260`, alloc `0x44`) y 5 corre `0xCE4` = 3300 frames.  `CGameMan`
  exige los paks PLAYER/POWERUP/STAGE (errores 1034–1036).
* **Medido con el log nuevo de `CDemoPlayMan`** (`bootdbg: ... DEMO`):
  state 0→1→2→3 en los presents 4298–4301, state 4 en el 4416 y **nunca
  llega a 5** hasta el 9000; el boot manager sigue en sub 9 y la pantalla
  blanca se mantiene.  El estado 4 es el que aloja y llama a `CGameMan`
  (`0x4142B0(0x5E1F28,0x44)` + `0x42E1F0`): hay que ver si esa llamada no
  retorna (SEH), si `CGameMan` deja `mask220=0xFFFFFFFF` (algún 1034–1036)
  o si el manager deja de actualizarse.  `OTOMEDIUS_BOOTDBG=1` ya imprime
  `DEMO state`, `slot25c`, `slot260`, `io` y `mask220` en cada cambio.
* El boot manager (`0x4547E0`) está en **state=0 sub=9** desde el present
  4299 esperando a que `CDemoPlayMan` libere `0x5DE258`.  Por tanto la
  pantalla blanca no es un draw perdido: es el fade mientras el demo carga
  y arranca.

### 10.5 Ficheros de esta sesión

```
M native/d3dx9.c          decode_dds por bits/máscaras; R8G8B8/X8/X1
M native/d3d9_objects.c   TF_R8G8B8/TF_X8R8G8B8/TF_X1R5G5B5; R5G6B5 real
M native/dsound.c         mezclador dsound→SDL, formato/posición/loop, dump
M native/movie.c          el ffmpeg de audio ya no usa -re (cola llena, sin choppy)
M native/platform_sdl.c   OTOMEDIUS_WINDOW_SIZE; mutex de audio; dump al salir
M native/platform.h       (sin cambios de API; se usa platform_audio_*)
M native/main.c           dsound_shutdown() antes de platform_shutdown()
M native/win32.h          prototypes dsound_shutdown / render_dump_fb
M native/render.c         render_dump_fb(); diagnósticos DISCARD/SKIP y
                          matrices WORLD/VIEW/PROJ
M docs/HANDOVER.md        esta sección
A docs/HANDOVER_PROMPT.md documento de transferencia de 6 secciones para la
                          sesión siguiente (copiar/pegar como prompt inicial)
```

Pendiente tras esta sesión: dejar correr el attract más allá del fade de
carga (present > 6200) para ver el eye-catch y el demo (`CGameMan`), añadir
el camino XYZ + transforms + z-buffer si el demo usa 3D, y subir el mezclador
al Flatpak (el hilo de audio necesita el dispositivo PulseAudio del sandbox).

---

### Atribución

La recompilación estática, la ingeniería inversa y la capa nativa Linux se
desarrollan con varios LLMs usados a través de **OpenCode** como agente de código.
