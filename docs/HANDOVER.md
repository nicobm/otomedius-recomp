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
> El bloqueo activo es el **layout/UV del atlas de `CTexFont`**: el texto se
> dibuja pero sale como ruido. Lee §5 y §6 primero. Este documento es el prompt
> de transferencia para una sesión nueva; el repo es
> `/var/home/vm/Proyectos/SALA DE TRABAJO/otomedius-recomp`.

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
* Helper scripts written this session (outside the repo, `/tmp/opencode/`):
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
OTOMEDIUS_NO_LOADER_GUARD=1      # do not force the async loader to finish
OTOMEDIUS_LOADER_GUARD=1         # force it on a 7 s stall (now also default, see §4.7)
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
OTOMEDIUS_DIR=path               # game directory
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
| `0x00419330` / `0x004193A0` | loader ring helpers that set **state 3** (`0x419355` / `0x4193CC`) |
| `0x00419D00` | **async loader thread body**: `EnterCriticalSection(0x5DE070)`, scan the ring for `[slot+4] != -1`, run `0x418040(slot[4], &slot[8])`, `LeaveCriticalSection`, loop; on "no work" sets `[0x5B3F82]=1` and calls `_endthreadex` |
| `0x00419530` | **"has the load finished?" poll** (called by the manager): `Sleep(16)`, `eax = 0x419430()`, returns **1** when the pump returns 0 or 2, else 0 |
| `0x00419430` | **loader pump** (main thread): compares ring head/tail, polls `[0x5BC914]` with `WaitForSingleObject(h,0)`, closes it, spawns a loader via `0x419CA0`, sets state 0 when head==tail |
| `0x00419CA0` | spawns one loader thread (`_beginthreadex(0x419D00)`), `SetThreadPriority(-2)`, `ResumeThread`, `[0x5BC91C]=1` |
| `0x00419E30` | **job gate**: returns 1 only if `[0x5BC914] != 0 && [0x5BC91C] == 3`; used by `0x418040` |
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
* Because the pump's handshake never completes under our threading emulation
  (`head=1`, `tail=2`, the worker handle never reports done), `0x419530` is
  answered "finished" after **70 polls (~7 s)** with no progress. Default on;
  `OTOMEDIUS_NO_LOADER_GUARD=1` restores waiting. This is what makes the boot
  machine run at all.

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

### 5.2 Bloqueo activo: el layout/UV del atlas de `CTexFont`

El texto ya **no es invisible** (el atlas tiene alpha y los glifos son
reales), pero se ve como **ruido/rayas**. Datos medidos:

1. Con FreeType el atlas 512×64 tiene **22 717 píxeles** con alpha
   (`first=17ffffff`, `alpha0=0`). El atlas anterior sintético tenía 324.
2. El atlas es **denso**: un único componente conexo `(0,18)-(511,63)` con
   22 299 píxeles; la celda `(288,24)` de 48×6 tiene **273/288** píxeles no
   cero. Comparado con el atlas sintético (cajas huecas), esto demuestra que
   los glifos reales se están escribiendo unos encima de otros.
3. La tabla de fuente del juego (`0x5BE7CC`, entrada del font index 1) es:
   `+0x00=0` (string), `+0x04=0x60` (96), `+0x08=24.0`, `+0x0C=24.0`,
   `+0x10=0.09375`, `+0x14=0.09375`, `+0x18=0x10004100` (glyph array).
   `0.09375 = 48/512 = 6/64`: la celda lógica es **48×6 px**.
4. Los glyph records son `N=(6,4)`, `O=(7,4)`, `W=(5,5)`, `I=(1,4)`,
   `T=(2,5)`, `A=(3,3)`, `L=(4,4)`, `Z=(8,5)`, `G=(9,3)`.
5. El quad de texto pasa `u=6.0..6.09375`, `v=4.0..4.09375`: un **span de
   celda** de `0.09375` en U (48 px) y `0.09375` en V (6 px).
6. El atlas empaqueta glifos de **~6 px** de ancho (`gmCellIncX` a `h/4` es
   ~3 px, a `h=24` ~13 px), pero el span U del quad es **48 px**. Cada quad
   muestrea entre 4 y 8 glifos en vez de uno → ruido.

Hipótesis, en orden:

1. El **packer de `CTexFont`** (`0x409C00` → `0x409D20`) avanza el lápiz X
   con `gmCellIncX` (o con el ancho del glifo), no con el ancho de celda de
   48 px. Hay que instrumentar `0x409D20` y volcar `[edi+0x28]` (pen X),
   `[edi+0x2C]` (pen Y), `[edi+0x4C]`/`[edi+0x50]` (ancho/alto del glifo)
   para cada carácter y compararlo con las UVs de los glyph records.
2. El **tamaño de la `FT_Face`** no es el correcto. Ahora se usa `h/4`
   (24 → 6 px) porque la celda V es 6 px; pero la celda U es 48 px. Probar
   `h` y `h*2` y medir el atlas/pen. La proporción 4× puede ser solo vertical.
3. La **convención UV** del juego puede necesitar una **texture matrix** que
   no se está aplicando (`SetTransform` no se llama; `D3DTSS_TEXTURETRANSFORMFLAGS`
   se pone a 0). El remapeo actual en `draw_up_vertices` es una aproximación.
4. El **7º argumento** de `GetGlyphOutlineA` es un segundo
   `GLYPHMETRICS`-shaped buffer en `&[font+0x3A]`. El juego lo precalcula como
   offsets UV; **no se debe espejar** el `gm` de la API ahí (el espejo está
   desactivado en `gm_mirror`).
5. El rasterizador `0x409D20` tiene dos caminos seleccionados por
   `[font+0x38]` y `[font+0x39]` (bitmap/outline y 1bpp/4bpp). Hay que
   confirmar que GGO_NATIVE entra por el camino de líneas y no por el de
   bitmap.

El remapeo actual de UVs de celda en `native/render.c` es:

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
`6.09375` da `0.65625` (texel 336): un span de 48 texels. Si el atlas real
empaqueta a 6 px, este span es 8× demasiado grande.

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
* **Japonés**: la fuente por defecto es DejaVu Sans (ASCII). Para CJK usar
  `OTOMEDIUS_FONT=/ruta/NotoSansCJK.ttc` o similar; el runtime tiene DejaVu
  en `/usr/share/fonts/dejavu/DejaVuSans.ttf`, y `/run/host/fonts` puede
  aportar Noto CJK si el sandbox lo permite.
* **4 quads con coordenadas enormes**: bbox `(-303352,8)-(2926,319040)`.
  Están fuera de pantalla y no bloquean, pero conviene identificar el draw
  (posiblemente el marco de la consola con una escala mal calculada).

Ficheros modificados/nuevos en el working tree (checkpoint sin commit al
escribir esto; el commit se hará al cerrar la sesión):

```
M io.github.nicobm.OtomediusRecomp.json
M native/d3d9.c
M native/d3d9_objects.c
M native/patches.c
M native/platform_sdl.c
M native/render.c
M native/win32_misc.c
M native/x86rt.c
M tools/recomp/emit.py
?? native/font.c
?? native/font.h
```

`docs/HANDOVER.md` y `README.md` también se actualizan en el checkpoint.

Artefactos locales (fuera del repo): `.work/gen` regenerado (85 ficheros con
`--chunk 60`), `.work/obj` reconstruido, `.work/freetype_include` y
`.work/freetype_lib` extraídos de FreeType 2.13.2 para el contenedor del
agente. El Flatpak regenera **42** ficheros `rc_*.c` porque el manifiesto no
pasa `--chunk 60`; no es un bug, pero explica la diferencia con los 85
locales.

## 6. Plan de Acción Inmediato

### Paso 1 — Recompilar el Flatpak y verificar FreeType

El Flatpak instalado es **anterior** a FreeType. Recompilar:

```sh
bash build-game-flatpak.sh "/ruta/al/Otomedius (2008-04-18)(JPN)[Konami PC][TP].zip"
flatpak run io.github.nicobm.OtomediusRecomp
```

Comprobar en la salida:

```
font: using /usr/share/fonts/dejavu/DejaVuSans.ttf
d3d9: tex LockRect(60040490) 512x64 -> ...
render: frame 360: ... tex0=60040490 bbox=(448,480)-(832,536)
```

Si no aparece `font: using`, revisar que el SDK tenga
`pkg-config --modversion freetype2` (26.7.20) y que el manifiesto pase
`$(pkg-config --cflags freetype2)` y `$(pkg-config --libs freetype2)`.

### Paso 2 — Resolver el layout/UV del atlas (bloqueo real)

1. Instrumentar el packer `0x409C00`/`0x409D20` (watch + memoria) y volcar,
   por carácter: `[edi+0x28]` (pen X), `[edi+0x2C]` (pen Y),
   `[edi+0x4C]`/`[edi+0x50]` (ancho/alto), y las UVs del glyph record.
2. Comparar con la tabla de fuente (`0x5BE7CC`, entrada 1): `+0x10=0.09375`,
   `+0x14=0.09375`; decidir si la celda es 48×6 o 6×6.
3. Probar variantes controladas:
   - `FT_Set_Pixel_Sizes` a `h/4`, `h`, `h*2` (ya está `h/4`).
   - `LINE_CHUNK=8` (líneas cortas; ya está) frente a `QSPLINE`.
   - desactivar/activar el remapeo de celdas de `render.c`.
   - `OTOMEDIUS_TEXDBG=1` para volcar `tex_60040490.pgm` y analizar
     componentes/ocupación de celdas.
4. Objetivo de aceptación: el atlas debe ser **disperso** (glifos separados)
   y el frame debe mostrar “NOW INITIALIZING” legible en
   `(448,480)-(832,536)`. Si el packer no se puede domar, alternativa:
   **bypass** del atlas: generar un atlas 512×64 con FreeType en la rejilla
   exacta que espera el juego y sustituir la textura tras `LockRect`.

### Paso 3 — Primera escena real

Con el texto legible, seguir la carga de escena:

```sh
OTOMEDIUS_LOG=1 OTOMEDIUS_WATCH="45eb20:2,45ff10:2,439a8c:2" \
  ../obj/otomedius-recomp .
```

Buscar `CreateFileA(./Data/Title.pak)`, `Warning.pak`, `KonamiLogo.pak`.
Revisar el manager `0x42D3D0` (sub-estado `[manager+8]`, tabla `0x42DBA4`) y
la creación de escena `0x42D9F8`. Cuando se entienda el vector de packages,
retirar el guard de `0x418190` y el de `0x414920` y validar el commit real.

Después de esos 3 pasos: renderer de sprites reales
(`0x40F780`/`0x40FA44`), audio (`dsound` → PulseAudio), y carga de pak más
rápida (buffer de lectura en `k_ReadFile`).

---

### Prompt corto para pegar al abrir la sesión nueva

```
Contexto: repo /var/home/vm/Proyectos/SALA DE TRABAJO/otomedius-recomp
(recompilación estática de Otomedius, Flatpak + SDL3). Lee docs/HANDOVER.md
entero antes de tocar nada.

Estado: el juego arranca y es estable 30 s (exit 124, 0 crashes). x87
corregido (fxch/pop), CreateDevice 1280x1024, texturas reales por defecto,
atlas 512x64 se construye y sube, FreeType integrado (DejaVu, h/4, mutex).
BLOQUEO: el atlas de CTexFont se llena de glifos solapados; los quads de
texto en (448,480)-(832,536) muestrean 48 px de U (0.09375) mientras el
atlas empaqueta a ~6 px -> ruido. Plan: instrumentar 0x409C00/0x409D20 y
la tabla 0x5BE7CC, ajustar face size/UV, validar con OTOMEDIUS_TEXDBG.
Después: escena real (0x45EB20, 0x45FF10, 0x439A8C) y retirar los guards de
0x418040/0x418190/0x414920.

No reescribas el HANDOVER sin pedirlo. No hagas commits sin pedirlo.
Build local: sh /tmp/opencode/build.sh <archivo.c ...>; gen:
PYTHONPATH=../.work/pylib python3 tools/recomp/recomp.py --exe
../.work/oto/otomedius.exe --out ../.work/gen --chunk 60; run:
cd ../.work/oto && ../obj/otomedius-recomp .
```

### Atribución

La recompilación estática, la ingeniería inversa y la capa nativa Linux se
desarrollan con **DeepSeek V4.1 Flash 1017**, servido por **OpenRouter** y
usado a través de **OpenCode** como agente de código.