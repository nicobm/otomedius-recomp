# Otomedius (Arcade) — Technical Handover

> Static recompilation of the 2008 Konami arcade shooter **Otomedius** to native
> Linux x86-64, packaged as a Flatpak. **This repository contains no game data**;
> the original executable is translated on the user's machine from their own
> dump. Repository: `https://github.com/nicobm/otomedius-recomp` (branch
> `master`).
>
> Status: **the game boots end-to-end** (loader, Konami boot machine, NVRAM
> config, scene instantiation) but **nothing is visible yet** because the font
> atlases are never uploaded. That is the single active blocker. §5 and §6 are
> the parts to read first.

---

## 1. Environment and toolchain

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
  `chk.py`, `xtr*.py` (binary xref scanners).

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
  'f="$1"; cc -O2 -std=c11 -w -Inative $(pkg-config --cflags sdl3) -c "$f" -o "out/obj/$(basename "$f" .c).o"' _ {}
find recomp/gen -name 'rc_*.c' -print0 | xargs -0 -P "$(nproc)" -I{} sh -c \
  'f="$1"; cc -O2 -std=c11 -w -Inative -c "$f" -o "out/obj/$(basename "$f" .c).o"' _ {}
cc -O2 -o out/otomedius-recomp out/obj/*.o -lm -lpthread $(pkg-config --libs sdl3)
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
OTOMEDIUS_TEXTURES=1             # real IDirect3DTexture9::LockRect + surface sharing
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
OTOMEDIUS_DIR=path               # game directory
```

---

## 2. Binary and memory map

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

## 3. Recompiled functions and solved subsystems

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

---

## 4. Patches and workarounds applied

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

### 4.10 GDI font path (`win32_misc.c`)

`CreateFontA` stores the requested height; `GetTextMetricsA` and
`GetCharABCWidthsA` return coherent metrics; **`GetGlyphOutlineA` implements
`GGO_NATIVE`** (a closed rectangle contour in `TTPOLYGONHEADER`/`TTPOLYCURVE`
format, `cb = 52`, 16.16 fixed point, +y upwards) and `GGO_BITMAP`
(1bpp box). Font face names are logged converted **CP932 → UTF-8** (they are
Shift-JIS: `ＤＦＧ中太丸ゴシック体`).

---

## 5. Current state and active blocker

### 5.1 What works (verified headless and in the Flatpak)

```
loader: image 00400000-005e9000 entry 00530e2b
imports: 300 bound, 0 missing
config: oto.ini: 85 values applied, 0 unknown
config: async loader ring initialised (16 free slots)
platform: opengl renderer
main: entering 00530e2b
user32: CreateWindowExA(1280x1024) wndproc=00401bb0
avs: ac_io_avs_boot
d3d9: Direct3DCreate9(32) -> 60000090
d3d9: CreateDevice(1024x768) windowed=0
kernel32: CreateFileA(././HidBoot.id) -> fd
kernel32: CreateFileA(./oto.ini) -> fd
dsound: DirectSoundCreate8 -> 60040420
kernel32: CreateFileA(././Data/otomedius.xgs) -> fd
kernel32: MapViewOfFile(116 bytes) -> 0a000000
gdi32: CreateFontA(h=48 face=ＤＦＧ中太丸ゴシック体)
gdi32: CreateFontA(h=28 face=ＤＦＧ中太丸ゴシック体)
kernel32: CreateFileA(././ChkSum.bin) -> fd
render: frame 120: clear=1 draw=0 up=16 verts=32 fvf=000144 … bbox=(0,-31)-(360,24)
kernel32: CreateThread(slot=0 handle=00001000 fn=0041b2a0)      # audio mixer
kernel32: CreateThread(slot=1 handle=00001010 fn=00419d00)      # loader
kernel32: CreateFileA(././Data/Once.pak) -> fd
kernel32: MapViewOfFile(29883830 bytes) -> 0a001000
config: loader stalled in state 1 after 52 polls, forcing completion
config: package registry empty, skipping the finalise step
avs: bms_init -> 0 ; bms_init_wait -> 0 ; bms_eventlog_request_init -> 0
kernel32: CreateFileA(./testmode.bin) -> fd                    # LoadConfig
kernel32: CreateFileA(./bookkeep.bin) -> fd                    # LoadBookKeep
gdi32: CreateFontA(h=24 face=ＤＦ中太丸ゴシック体)
x86: thread end   00530547 (ExitThread)
render: frame 360: clear=1 draw=0 up=25 verts=50 fvf=000144 … tex0=60040490 bbox=(-631,-5)-(12,0)
```

Verified state after the boot: `[0x5E2027] = 0xFF` (load done),
`[0x5E1F96] = 2` (boot machine ready), `[0x5E1FA5]=5`, `[0x5E1FA6]=3`,
`[0x5E1FA7]=2`, `[0x5DE220]=0` (no manager suspended), the scene manager
`[0x5DE228]` alive and its sub-state `[manager+8] = 2`, scene objects created
once (`0x45F1B0`, `0x439C80`, `0x44D190`).

Window opens 1280×1024, `Esc`/close quit cleanly through `WM_QUIT`, no crashes,
`exit 124` under `timeout`.

### 5.2 The active blocker: the font atlases are never uploaded

What the user sees: **black screen**, a few white pixels top-left that animate
for ~4 s and then freeze.

Measured facts:

1. The UI is drawn as **25 `DrawPrimitiveUP` per frame** (`draw=0`: the game
   never uses `DrawPrimitive`), all with `tex0 = 0x60040490`.
2. `0x60040490` is **`CreateTexture(512x64, fmt 21 = A8R8G8B8)`** — a **font
   atlas**. `0x60060570` (512x256) is the second one. **Neither is ever
   uploaded**: no `texture … uploaded` line is emitted for them (the game-side
   upload path is `LockRect`, which by default still returns
   `D3DERR_INVALIDCALL`).
3. An all-zero atlas means alpha 0 everywhere ⇒ the rasteriser blends nothing ⇒
   the text is invisible. The few white pixels are the untextured fragments.
4. The quads' coordinates are `(-403.5, 0.3) … (12.0, -5.7)` (415×6 px bars
   anchored at `(12,0)`), i.e. **above the top edge**. This is not a viewport
   bug: the only viewport is `(0,0,1024,768)`; it is an entry animation that
   never advances because the UI is invisible/frozen.
5. `[manager+8]` is written **exactly once** (0 → 2) in the whole run
   (`OTOMEDIUS_WATCHCHG=auto`), so the manager stays in the boot sub-state by
   design; the real scene state lives in the scene object.

**Why the atlas is empty.** The game builds font atlases with GDI:
`CreateFontA` → `SelectObject` → `GetTextMetricsA`/`GetCharABCWidthsA` →
**`GetGlyphOutlineA` with `fuFormat = 6` (`GGO_NATIVE|GGO_GRAY2_BITMAP`)** →
rasterise the outlines itself (`CTexFont`) → upload via
`IDirect3DTexture9::LockRect` (through the level-0 surface).

With **`OTOMEDIUS_TEXTURES=1`** (real `LockRect` + surface sharing) the game
attempts the atlas build and then **crashes**:

```
x86: no recompiled code at 00000000 (eip=00000000 esp=009fee30 ebp=009feeac
                                    eax=00000000 ebx=00000030 ecx=00000000)
last returns: 005326de -> 00532a92 / 0053d26e -> 00532a65  (CRT region)
```

i.e. a call through a NULL pointer inside the `CTexFont` rasteriser
(`0x409C00` region) / CRT helpers `0x532xxx`, while consuming the synthetic
`GGO_NATIVE` contour. Hypotheses (in order):

1. Our `GGO_NATIVE` output is not what `CTexFont` expects (contour count, `cb`
   fields, fixed-point rounding, or it needs `GGO_GRAY2_BITMAP` too).
2. The level-0 surface/`LockRect` path still disagrees with the game (pitch,
   format, or the texture is locked while a surface of a different size is used).
3. The `GetTextMetricsA`/`GetCharABCWidthsA` numbers are coherent but wrong
   enough that the atlas layout overflows and corrupts a pointer.

---

## 6. Immediate action plan (next 3 steps)

1. **Fix the `OTOMEDIUS_TEXTURES=1` crash in the font path.** Reproduce headless
   (`cd ../.work/oto && OTOMEDIUS_TEXTURES=1 ../obj/otomedius-recomp .`), capture
   the trace (`last dispatch targets` / `last returns`), and identify which call
   receives the NULL pointer. The last run showed the fault after CRT helpers
   `0x5326de -> 0x532a92` / `0x53d26e -> 0x532a65` with `eax=ecx=esi=edi=0`
   (`esp=009fee30`); the game-side suspect is the `CTexFont` rasteriser around
   `0x409C00`–`0x409Exx`. Use `OTOMEDIUS_WATCH` on the `CTexFont` entry points
   plus `OTOMEDIUS_WATCHMEM`/`PEEKREGS` to dump the `this` object. Expected
   payoff: the atlas gets built and uploaded, and text becomes visible.

2. **Make the glyph path faithful.** Either
   (a) adjust `g_GetGlyphOutlineA` (`native/win32_misc.c`) so the returned
   `TTPOLYGONHEADER`/`TTPOLYCURVE` stream matches what `CTexFont` consumes
   (compare against a real GDI dump; check `cb`, `dwType = 24`,
   `wType = TT_PRIM_LINE = 1`, `cpfx`, 16.16 fixed point, +y up), or
   (b) replace the DC font with a **real** one (FreeType + a system TTF), which
   also fixes Japanese text later. Then confirm with
   `OTOMEDIUS_LOG=2 | grep 'texture 60040490'` that the 512×64 atlas is uploaded,
   and dump a frame (`OTOMEDIUS_DUMP_FB`) to verify pixels appear.

3. **Then chase the first real scene.** Once text is visible, watch
   `0x45EB20` (Title), `0x45FF10` (Warning), `0x439A8C` (KonamiLogo) and the
   scene object created by `0x42D9F8` (vtable + which methods are called per
   frame). `[0x5E2027]` is already `0xFF`, so the manager will start loading
   scene paks as soon as the UI/transition advances. Milestone: a
   `CreateFileA(./Data/*.pak)` for a scene (not `Once.pak`).

After that: finish the renderer for real content (the sprite path
`0x40F780`/`0x40FA44`: log vertex data at `DrawPrimitive`/`DrawIndexedPrimitive`),
audio mixing (`dsound` → PulseAudio), and speed up pak loading (`ReadFile` is
called in 1024-byte chunks by the CRT `fread`; a read-behind buffer in
`k_ReadFile` would help).

---

### Useful commands

```sh
# Flatpak build + run
bash build-game-flatpak.sh /path/to/otomedius.zip
flatpak run --env=OTOMEDIUS_LOG=1 io.github.nicobm.OtomediusRecomp

# local headless iteration (agent environment)
cd "<SALA DE TRABAJO>/.work"
zig/zig cc -std=c11 -O1 -w -I../otomedius-recomp/native -c ../otomedius-recomp/native/X.c -o obj/X.o
gen: PYTHONPATH=../.work/pylib ../otomedius-recomp/tools/recomp/recomp.py --exe oto/otomedius.exe --out gen --chunk 60
run: cd oto && timeout 20 env OTOMEDIUS_LOG=1 ../obj/otomedius-recomp .
```

### Attribution

The static recompilation, the reverse engineering and the native Linux layer in
this repository are being developed with **DeepSeek V4.1 Flash 1017**, served by
**OpenRouter** and driven through **OpenCode** as the coding agent.
