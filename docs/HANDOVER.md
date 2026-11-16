# Otomedius (Arcade) — Technical Handover

> Static recompilation of the 2008 Konami arcade shooter **Otomedius** to native
> Linux x86-64, packaged as a Flatpak. **This repository contains no game data**;
> the original executable is translated on the user's machine from their own
> dump. Repository: `https://github.com/nicobm/otomedius-recomp` (commit
> `a729406`, branch `master`).

---

## 1. Environment and toolchain

### Target
| Item | Value |
|---|---|
| Host / target OS | Linux **x86-64 only** (no Windows, no macOS, no 32-bit build) |
| Delivery | Flatpak, runtime `org.freedesktop.Platform//26.08`, SDK `org.freedesktop.Sdk//26.08` |
| Display | Wayland native (`--socket=wayland`), XWayland fallback (`--socket=fallback-x11`) |
| Sandbox | `--socket=pulseaudio --device=dri --device=input --nofilesystem=home --nofilesystem=host --unshare=network` (**`--share=ipc` intentionally removed**) |
| UI toolkit | **SDL3 3.4.20** (built as a Flatpak module from upstream tarball, sha256 `f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f`) |
| Recompiler deps | Python 3 + **capstone 5.0.9** (wheel `capstone-5.0.9-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl`, sha256 `273fd8d747d2e35c88f91450be51a603ecfaafb00d96d9f315dcb8689c86193e`, fetched by the wrapper into `flatpak/wheels/`, gitignored) |
| C compiler in Flatpak | SDK `cc` with `-O2 -std=c11 -lm -lpthread $(pkg-config --cflags/--libs sdl3)` |

### Local (agent) build, no Flatpak
* Compiler: **zig 0.17.1** downloaded to `<SALA DE TRABAJO>/.work/zig/zig` (`zig cc`).
* Headless backend: `tests/platform_null.c` implements every `platform_*` symbol (no SDL, no GPU).
* Layout used locally: `<.work>/gen` (generated C), `<.work>/obj` (native `.o`), `<.work>/obj/gen` (generated `.o`), `<.work/oto>` (extracted dump for testing).
* Run: `cd .work/oto && ../obj/otomedius-recomp .`

> **CRITICAL BUILD RULE:** `cpu_t` lives in `native/x86rt.h` and is embedded in
> every generated file. **Any change to that header requires rebuilding ALL
> objects, including the ~42 generated `rc_*.c`.** Mixing stale objects produced
> silent corruption and wasted hours (boot-time SIGSEGV at a "heap" address).
> The Flatpak build always compiles from scratch, so it never had this problem.

### Build pipeline
`build-game-flatpak.sh` (bash, distro-agnostic: prints `dnf`/`apt`/`pacman`/
`zypper`/`apk`/`xbps` hints when `flatpak`, `flatpak-builder`, `python3`,
`unzip`, `curl` are missing):

1. Locates the dump (arg, `game.zip`, or any `./*.zip`), verifies
   `sha256 == 4c6562eab3fff1bd1a6e84732bcb565a871440fcb894985fe990fe566a8bd897`
   (warning only), copies it to `game.zip`.
2. Installs runtime + SDK 26.08 (user remotes first, then system, then adds Flathub).
3. Downloads the capstone wheel (sha256-checked).
4. `flatpak-builder --user --install --force-clean --disable-rofiles-fuse --repo=repo build-dir io.github.nicobm.OtomediusRecomp.json`

Manifest `io.github.nicobm.OtomediusRecomp.json`, module `otomedius-recomp`
`build-commands` (single script, no helper scripts):
```
python3 tools/recomp/recomp.py --exe data/otomedius.exe --out recomp/gen \
        --expect-sha256 "$(cat recomp/exe.sha256)"
find native -name '*.c' -print0 | xargs -0 -P "$(nproc)" -I{} sh -c \
  'f="$1"; cc -O2 -std=c11 -w -Inative $(pkg-config --cflags sdl3) -c "$f" -o "out/obj/$(basename "$f" .c).o"' _ {}
find recomp/gen -name 'rc_*.c' -print0 | xargs -0 -P "$(nproc)" -I{} sh -c \
  'f="$1"; cc -O2 -std=c11 -w -Inative -c "$f" -o "out/obj/$(basename "$f" .c).o"' _ {}
cc -O2 -o out/otomedius-recomp out/obj/*.o -lm -lpthread $(pkg-config --libs sdl3)
```
Game data → `/app/share/otomedius-recomp/game/`; launcher
`flatpak/otomedius-recomp-launch` copies it once to
`$XDG_DATA_HOME/otomedius` (writable), and **copies `otomedius.ini` to
`oto.ini`** because the game opens `oto.ini` (the dump ships `otomedius.ini`).

### Directory structure
```
otomedius-recomp/
├── README.md  FLATPAK.md  LICENSE (MIT © 2026 nicobm)  VERSION (0.1.0)
├── build-game-flatpak.sh                 # the only user-facing script
├── io.github.nicobm.OtomediusRecomp.json # Flatpak manifest (3 modules:
│                                         #   python-capstone, sdl3, otomedius-recomp)
├── flatpak/
│   ├── otomedius-recomp-launch           # copies game data, creates oto.ini, execs
│   ├── io.github.nicobm.OtomediusRecomp.desktop / .svg / .metainfo.xml
│   └── wheels/                           # capstone wheel (gitignored)
├── native/                               # the Linux system layer (15 files, ~5.8k lines)
│   ├── main.c            boot, fault handler, watchdog, x86_thread_begin, TLS helpers
│   ├── memory.c          MEM mmap, PE image load, TEB/PEB setup
│   ├── imports.c         PE import binding, native thunks, x86_native_call/tail/invoke
│   ├── x86rt.h/.c        cpu_t, dispatcher, ALU/shift/string helpers, x87, SSE2
│   ├── win32.h           shared helpers (ARG32, RET32, log_printf…)
│   ├── win32_kernel32.c  files, heaps, vm, threads, TLS, code pages (iconv), time, mappings
│   ├── win32_user32.c    window + message queue
│   ├── win32_misc.c      gdi32, ole32/oleaut32/shlwapi/imm32, winmm
│   ├── d3d9.h/.c         IDirect3D9 (17 methods) + IDirect3DDevice9 (119), D3DCAPS9
│   ├── d3d9_objects.c    IDirect3DTexture9(22)/Surface9(17)/VertexBuffer9(14)/VertexShader9(5)
│   ├── d3dx9.c           matrix/vector math + DDS textures + stubs
│   ├── dsound.c          IDirectSound8(12) + IDirectSoundBuffer8(24)
│   ├── dshow.c           IGraphBuilder(18)/IMediaControl(16)/IMediaEvent(13)/IMediaEventEx(16)/
│   │                     IVideoWindow(46)/IBasicAudio(11)/IMediaPosition(18)/IMediaEventSink(4)/
│   │                     IBaseFilter(15)/IPin(19)
│   ├── avs.c             Konami libacio/libbms/libtouchpanel replacement
│   ├── render.c/h        renderer front-end (forwards to platform_*)
│   └── platform.h/_sdl.c SDL3 window, events, input
├── tools/recomp/         pe.py (own PE parser), xdis.py (discovery), emit.py (x86→C), recomp.py
├── recomp/exe.sha256     c55796ad1f2d6ea9bd0fb0d33ed6b6eb6463d0e3ad8f2e1d7c4608ca9dfcf112
├── tests/platform_null.c headless backend
└── docs/HANDOVER.md      this file
```

---

## 2. Binary and memory map

### The dump
* `Otomedius (2008-04-18)(JPN)[Konami PC][TP].zip` — sha256
  `4c6562eab3fff1bd1a6e84732bcb565a871440fcb894985fe990fe566a8bd897` (~452 MB).
* Contents: `otomedius.exe`, `otomedius_edit.exe`, `bms_GGG.exe`, `d3dx9_32.dll`,
  `libacio.dll`, `libavs-win32.dll`, `libbms.dll`, `libtouchpanel.dll`,
  `dbghelp.dll`, `HIdGet.exe`, `HidBoot.id`, `ChkSum.bin`, `bookkeep.bin`,
  `ea_conf.bin`, `testmode.bin`, `card_allow.list`, `otomedius.ini`,
  `Data/*.pak` (Microsoft Cabinet `MSCF`, MSZIP raw-deflate blocks),
  `Data/*.xwb` (xWMA), `Data/OTOMEDIUS_OP_f.wmv`, `Data/*.dds` embedded in paks.

### otomedius.exe
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

Import DLLs: `d3d9.dll`(1), `d3dx9_32.dll`(34), `imm32.dll`(1), `winmm.dll`(5),
`libacio.dll`(32), `libtouchpanel.dll`(13), `libbms.dll`(66),
`dsound.dll`(ordinal **11** = `DirectSoundCreate8`), `kernel32.dll`(104),
`user32.dll`(27), `gdi32.dll`(7), `ole32.dll`(6), `oleaut32.dll`(ordinals
**2**=`SysAllocString`, **6**=`SysFreeString`), `shlwapi.dll`(1).

### Emulated 32-bit address space (`native/memory.c`, `x86rt.h`)
| Range | Use |
|---|---|
| `0x00000000`–`0xFFFFFFFF` | one 4 GiB `mmap(MAP_NORESERVE)`; `MEM` points at it |
| `0x00400000`–`0x005E9000` | PE image at its original base |
| `0x000A0000`+ | CLI string, PEB fields |
| `0x02000000`+ | per-thread stacks (1 MiB each, slot N top = `0x02000000+(N+1)*1MiB`) |
| `0x0A000000`–`0x20000000` | `MapViewOfFile` arena (bump) |
| `0x10000000`–`0x30000000` | `HeapAlloc` arena (block headers `hdr_t{size,used,heap,magic=0x4F544D48}`) |
| `0x30000000`–`0x60000000` | `VirtualAlloc` arena |
| `0x60000000`+ | COM objects (`emu_alloc` bump): vtables + objects |
| `0x7F000000` | native thunks, **16 bytes apart**, `idx = (addr-0x7F000000)/16` |
| `0x7FF00000`+ | per-thread TEB (0x2000 each) |
| `0x7FFDE000` | main TEB (SEH head `+0x00`=`0xFFFFFFFF`, stack top `+0x04`, bottom `+0x08`, self `+0x18`, PEB `+0x1C`, pid `+0x24`, tid `+0x28`, TLS `+0x2C`, last error `+0x34`, TLS array `+0x1000`) |
| `0x7FFDF000` | PEB |

`X86_RET_SENTINEL = 0xFFFFFFF1` (returned from the entry point / thread body).

### Game globals already identified
| Address | Meaning / evidence |
|---|---|
| `0x005E1F1C` | second Direct3D object used in the caps/device-selection path (`0x4012f4`) |
| `0x005E1F20` | **`IDirect3DDevice9*`** (read by `0x402490`, `0x4024a0`, pusher for `D3DXCreateTexture` at `0x409884`) |
| `0x005E1F24` | frame-update gate: `cmp byte [0x5e1f24], bl; je 0x4025ae` at `0x402575` |
| `0x005E1F26` | byte checked after `timeGetTime` in the loop (`0x402682`) |
| `0x005E1F27` | **"rendering enabled"**, set by `WM_ACTIVATE` handler `0x401be1` (`test bx,bx; setne cl`), cleared on device loss |
| `0x005E1F50` | **HWND** |
| `0x005E1F58` / `0x005E1F59` | windowed / device-lost flags |
| `0x005E1F7C` | log callback object |
| `0x005E1F88` | `CSound*` (set by `0x404fb0`, read by `0x405120`) |
| `0x005E1F8C` | secondary sound object |
| `0x005E1F96` | **boot state machine index 0..4** (jump table `0x40c9fc`) |
| `0x005E1FA5` | I/O mode (`== 5` ⇒ call `ac_io_pjei_update_control_status_buffer`) |
| `0x005E1FA6` | touch panel mode (`== 3` ⇒ call `ac_touchpanel_renewal`) |
| `0x005E1FFD` / `0x005E1FFE` | video/IO enabled flags (`0x005E1FFE == 1` ⇒ `ac_io_update`) |
| `0x005E2000` | **touch-panel handle** (result of `ac_touchpanel_new`, passed back) |
| `0x005DCF88` | DirectShow/video manager object |
| `0x005DB477` | scene flag tested in `0x45eb20` (Title loader) |
| `0x005DE14C` | `IDirectSound8*` (out param of `DirectSoundCreate8` at `0x41a82e`) |
| `0x005DE128` / `0x005DE12C` | sound thread id / started flag |
| `0x005E6E60`, `0x005E6E28` | SSE2/runtime dispatch flags |

### Key constants
* Device caps checked by the game at `0x4012d9`: `MaxStreams >= 3`,
  `MaxPrimitiveCount >= 0xFFFF`, `MaxVertexIndex >= 0xFFFF`.
* `D3DFMT_X8R8G8B8 = 0x16`; `D3DERR_DEVICELOST = 0x88760868`;
  `D3DERR_DEVICENOTRESET = 0x88760869`; `VFW_E_TYPE_NOT_ACCEPTED = 0x8004022A`;
  `VFW_E_NOT_FOUND = 0x80040216`; `E_NOINTERFACE = 0x80004002`.

---

## 3. Recompiled functions and solved subsystems

### Recompiler (`tools/recomp`, own implementation, capstone-based)
* `recomp.py --exe X --out recomp/gen [--chunk 60] [--expect-sha256 H]`
* Discovery order: entry+exports+data-pointer seeds → fixpoint over call targets →
  immediate code pointers → **prologue heuristic last** → fixpoint of branch
  targets / call-return sites → **adoption** of data words pointing to unowned
  code (only if not inside an already decoded instruction).
* Jump tables: `jmp dword ptr [reg*4+table]` read strictly from `table`
  (a +4 bias attempt was tried and reverted: it broke `CSound::Init`).
* Output: **5 007 functions**, **380 029 instructions**, **95.4 %** of `.text`,
  **78 436 dispatch entries**, 42 files.
* Code model: one C function per x86 function, C `goto` for intra-function flow,
  `cpu->eip = target; return;` for inter-function transfers, dispatcher
  `x86_entries[]` (sorted, binary search) mapping address → (fn, continuation).
  `cpu->cont` selects the resume label; every return address is a table entry.
* Unhandled mnemonics (unreachable so far): `int3`(59), `in`(14), `out`(7),
  `les`(6), `aaa`(5), `hlt`(4), `jecxz`(2), `aam`(2), `lcall`(2),
  `cmpltpd`(2), `cmpeqsd`(2), `int`(1).

### Native runtime (`native/x86rt.*`)
`cpu_t`: 8 GPRs, eip, 6 flags, df, `cont`, `seg[6]`, `xmm[8][2]`, mxcsr,
x87 `st[8]` as `long double` + tag/top/cw/sw/cc, `teb`, `gs_base`,
`entry_esp`, `esp_check`. `x86_cpu()` is `__thread`.
Helpers: ALU with full flag semantics, shifts/rotates, `mul/div/imul`, string
ops (`x86_string`), `cpuid` (reports SSE2/SSE3, no SSE4/AVX), `rdtsc`,
`fs:`/`gs:` mapped to the TEB, full x87 set, SSE2 subset, `x86_call_stdcall`
(native → recompiled).

### Game functions identified so far
| Address | Role |
|---|---|
| `0x00530E2B` | `_WinMainCRTStartup` (calls `0x53b2ad`, then `jmp 0x530c4b`) |
| `0x0053B2AD` | `__tmainCRTStartup` (locale, heap, TLS, `ImmDisableIME`, calls WinMain) |
| `0x0051BA88` | thunk → `imm32!ImmDisableIME` |
| `0x0051BA10` | thunk → `d3dx9_32!D3DXCreateTexture` |
| `0x0052DB7A` | thunk → `dsound!DirectSoundCreate8` |
| `0x00401BB0` | **window procedure**; message jump table at `0x401D74`, byte
  index table `0x401D94` (`msg-2`); `WM_ACTIVATE(0x6)` → `0x401BE1`
  (`setne cl; mov [0x5e1f27], cl`), `WM_DESTROY(0x2)` → `0x401BF2` |
| `0x00401100` | CRT `WinMain` caller path (calls `ImmDisableIME`, pushes args) |
| `0x00402FB0`, `0x00402970`, `0x00402840`, `0x00402E90`, `0x00402760` | init helpers called before the boot check (`0x42d5ce`) |
| `0x00402230` | **shutdown path** (called from `0x4026bd` after the loop sees `WM_QUIT`): `0x439fb0`, `CSound::Move` (`0x4050b0`), `0x418190`, sound service `0x41a9a0` |
| `0x004024C0` | **frame update**: `TestCooperativeLevel` (device vtbl idx 3),
  `0x401a00`, `0x412fe0`, `0x439fe0`, `0x41aa10`, then `0x402340`; returns
  `al` and on failure the loop destroys the window |
| `0x00402340` | **render**: `0x40f780` (draw pass), `EndScene` (idx 42),
  `Present` (idx 17); returns Present's HRESULT |
| `0x00402620`–`0x004026BD` | **message loop**: `PeekMessageA`,
  `TranslateMessage`, `DispatchMessageA`, `0x4024c0`, `DestroyWindow`,
  `timeGetTime`; exits on `msg.message == 0x12` |
| `0x00402740`, `0x00403DB0` | printf-style log helpers |
| `0x00403070` | draw manager entry used by the frame |
| `0x0040C7F0` | **Konami boot state machine** (see §5): `Sleep(16)`,
  `ac_touchpanel_polling`, `bms_init` → `bms_init_wait` →
  `bms_eventlog_request_init` → `bms_is_active`; jump table `0x40C9FC` =
  `{0x40C85C, 0x40C8E1, 0x40C9C7, 0x40C9EB, 0x40C9F3}` |
| `0x0042D3D0` / call site `0x0042D5A0` | caller of the boot machine; **continues
  when the machine returns `al == 2` or `4`** (`0x42d5a7`/`0x42d5ac`), treats 3
  as error |
| `0x0040F780`, `0x0040FA44`, `0x0040E090`, `0x0040DF50`, `0x0040E2F8`,
  `0x0040E399`, `0x004053C9`, `0x004052E0` | drawing helpers (issue the device
  calls: `Clear` 43, `BeginScene` 41, `EndScene` 42, `SetVertexShaderConstantF`
  83, `SetSamplerState` 65, state blocks 57/56/58, `SetTexture` 61) |
| `0x00412EA0` | **I/O init**: `ac_io_begin(...)` → `CreateWindowExA`-ish →
  `ac_touchpanel_new` → `ac_touchpanel_carribrate_set` → sets `0x005E1FFD = 1`;
  returns `al = 1` |
| `0x00413CD0` | per-frame misc (touch panel poll, `DestroyWindow` on state) |
| `0x004175C0` | video object constructor (vtable `0x55926C`, 0x178 bytes) |
| `0x00417750` | video **texture creation** (vtable idx 36): `GetDeviceCaps`
  (idx 7), `CreateTexture` (idx 23) with `&this->0x160` |
| `0x00417A80` | **start movie**: `CoCreateInstance(CLSID_FilterGraph)`,
  `AddFilter(game filter)`, `AddSourceFilter(.wmv)`, `Render(pin)`,
  `QueryInterface(IMediaControl/…/…)`, `Run`, then reads `[video+0x160]` |
| `0x0041A820` | `CDirectSound::Init` (`DirectSoundCreate8`, `SetCooperativeLevel`,
  `CreateSoundBuffer`, `InitializeCriticalSection`, `_beginthreadex`, `Run`) |
| `0x0041A9A0` | sound service (waits on an event) |
| `0x0041B2A0` | **audio mixer thread entry** |
| `0x00404FB0` | `CSound::Create` (`MallocLocal(0x9c)`, `0x409810`, stores `0x5e1f88`) |
| `0x00405120` | `CSound::Init` (`0x4098f0` init, buffer setup) |
| `0x00409810` | sound device wrapper init (creates a texture via `D3DXCreateTexture`) |
| `0x00522580` / `0x005226E0` / `0x00522480` | the game's **TextureRenderer filter**
  `Pause` / `Run` / `Stop` (called by our graph) |
| `0x0045EB20` | **Title scene loader** – references `.\Data\Title.pak` (`0x567094`) |
| `0x0045FF10` | **Warning scene** – references `.\Data\Warning.pak` (`0x5671EC`) |
| `0x00439A8C` | references `.\Data\KonamiLogo.pak` (`0x55C884`) |

### Subsystems implemented natively
* **kernel32** – files (Win32 disposition semantics **OPEN_EXISTING=3 →
  `O_RDWR`, OPEN_ALWAYS=4 → `O_CREAT|O_RDWR`**), heaps, `VirtualAlloc`,
  threads (own stack + TEB per thread), TLS in the TEB, code pages via
  `iconv` (CP932/936/949/950/1252), `GetProcAddress` (real export walk),
  `CreateFileMappingA`/`MapViewOfFile` (real `pread` into the map arena),
  time, critical sections, events/semaphores.
* **user32** – one window (`HWND 0x00010001`), message queue, `RegisterClassExA`
  reads `lpfnWndProc` at **+8** (`WNDCLASSEXA`), `CreateWindowExA` sends
  `WM_CREATE`, `WM_SIZE`, `WM_ACTIVATE(1)`, `WM_SETFOCUS`; close/ESC push
  `WM_QUIT`.
* **d3d9** – `IDirect3D9` (17 methods) and `IDirect3DDevice9` (**119 methods**,
  exact order) built from `d3d9.h`; objects live in emulated memory with
  vtables of native thunks; every method's argument count verified.
* **d3d9_objects** – texture (22), surface (17), vertex/index buffer (14),
  vertex/pixel shader (5) with correct layouts.
* **d3dx9** – real maths (`MatrixMultiply/Translation/Scaling/Rotation*/
  LookAtLH/PerspectiveFovLH/Transpose/Inverse`, `Vec3*`, `QuaternionSlerp`,
  `PlaneTransform`, `Vec2Hermite`), `D3DXCreateTexture` (creates a D3D texture
  object), DDS decoding (`DDS ` header, DXT1/3/5 + uncompressed).
* **dsound** – `IDirectSound8` (12) + `IDirectSoundBuffer8` (24); buffers hold
  emulated sample memory, `Lock`/`Unlock`/`Play`/`Stop`/volume/pan work,
  **audio is silent** (no mixer yet).
* **dshow** – full fake pipeline: `IGraphBuilder`(18), `IBaseFilter`(15),
  `IPin`(19), `IMediaControl`(16), `IMediaEvent`(13), `IMediaEventEx`(16),
  `IVideoWindow`(46), `IBasicAudio`(11), `IMediaPosition`(18),
  `IMediaEventSink`(4). `AddFilter` → `JoinFilterGraph`; `Render(pin)` →
  `EnumPins` + `Next` + `ReceiveConnection` with a hand-built
  `AM_MEDIA_TYPE` (MEDIATYPE_Video, RGB32/YUY2, 1280×1024, VIDEOINFOHEADER,
  166667 ns/frame); `Run` → `SetSyncSource(NULL)` + `Pause` + `Run` on every
  added filter; `EC_COMPLETE` is delivered and the graph goes to Stopped.
* **avs** – Konami cabinet replacement (see §4).
* **render/platform** – `render.c` forwards to `platform_*`; `platform_sdl.c`
  opens an SDL3 window (1280×1024), polls events, maps keys/pad. Drawing calls
  are **no-ops** (screen stays black).

---

## 4. Patches and workarounds applied

### Konami AVS libraries (`native/avs.c`) — all **cdecl** (`nargs = 0`) and specific returns
| Function | Returns | Why |
|---|---|---|
| `ac_io_avs_boot` | `0` | cabinet I/O booted |
| `ac_io_begin` | **`1`** | the game tests `al` and aborts I/O init on 0 |
| `ac_io_is_active2` | **`0`** | boot state 1 loops while it is `1` |
| `ac_io_pjei_req_secplug_check` | **`0`** | 0 = security plug present → boot state 1 |
| `ac_io_pjei_req_secplug_check_isfinished` | `1` | finished |
| `ac_io_pjei_req_secplug_missing_check[_isfinished]` | `0`/`0` | no missing plug |
| `ac_io_icca_cardunit_init[_isfinished]` | `0`/`1` | card reader "present" |
| `ac_touchpanel_new` | **real `emu_alloc(64)` pointer** | the game keeps it and dereferences it |
| `ac_touchpanel_caribrate_set` | `0` | accepted |
| `ac_touchpanel_send_check_diag` | **`1`** | request sent |
| `ac_touchpanel_check_wait_receive` | **`0`** | with `0` the game proceeds to `receive_check_diag` |
| `ac_touchpanel_receive_check_diag` | `0` | reply OK |
| `ac_touchpanel_polling` | `0` | no touches |
| `bms_init` | `0` | → boot state 1 |
| `bms_init_wait` | `0` | → `bms_eventlog_request_init` |
| `bms_eventlog_request_init` | `0` | → boot state 2 |
| `bms_is_active` | **`1`** | with `0` the boot machine jumps back to state 0 (infinite loop) |
| everything else in libacio/libbms/libtouchpanel | `0` | free play, no network, no card |

Input for `ac_io_pjei_get_control_status_buffer`: 16 bytes, **active-low 16-bit
switch word** at offset 0 — `bit0 up, bit1 down, bit2 left, bit3 right, bit4..7
shot/bomb/option/special, bit8 start, bit9 coin` (layout is a guess, untested;
the game has not reached input polling yet).

### D3D9 workarounds
* `d9_GetDeviceCaps`: full realistic `D3DCAPS9` (SM 3.0 `0xFFFE0300`/`0xFFFF0300`,
  `MaxVertexShaderConst 256`, `MaxStreams 16`, `MaxPrimitiveCount 0x7FFFFF`,
  `MaxVertexIndex 0xFFFFFF`, `MaxTextureWidth/Height 8192`,
  `MaxSimultaneousTextures 8`, `DevCaps HWTRANSFORMANDLIGHT`). **pCaps is
  `arg32(3)`** (`this, Adapter, DeviceType, pCaps`) — reading arg 2 made the
  game reject every device type and never call `CreateDevice`.
* `GetAdapterIdentifier` → `arg32(3)`; `EnumAdapterModes` → `arg32(4)`.
* `d9_CreateDevice`: `ppDevice = arg32(6)`, size read from
  `D3DPRESENT_PARAMETERS` (`BackBufferWidth +0`, `Height +4`, `Windowed +32`).

### DShow workarounds
* The game's own "TextureRenderer" filter is the only filter in the graph; we
  drive it exactly like DirectShow would.
* Since no WMV sample ever arrives, **we create the video texture ourselves**:
  `IDirect3DDevice9::CreateTexture(device, 1280, 1024, 1, 0, 0x16, 0,
  &video+0x160, 0)` (device from `[0x5E1F20]`, video = `filter - 0xC`).
* `IMediaControl::GetState` mirrors the lifecycle (0 stopped / 1 paused /
  2 running); delivering `EC_COMPLETE` sets it to Stopped.

### Recompiler / emitter workarounds
* Function bodies that end without a terminator emit
  `cpu->eip = <next>; return;` ("falls through into the next function").
* `call`/`jmp` with a computed target go through `x86_jump()`, which aborts with
  a diagnostic when the target is 0 instead of ending the program silently.
* `jmp [IAT]` (import thunks) use `x86_native_tail()` = invoke the native
  function, pop its args **and the pushed return address**, then set
  `cpu->eip = ret`. (Emitting `eip = 0` here was why the game "returned"
  before `WinMain`.)
* **Vtable argument counts:** `STDMETHOD(Name)(THIS)` ⇒ **1** (only `this`);
  `STDMETHOD(Name)(THIS_ args…)` ⇒ **args + 1**. The old parser added +1 to
  both, so `AddRef`, `Release`, `BeginScene`, `EndScene`, `Stop`, `Unlock`
  popped 4 extra bytes per call — this caused the frame-loop stack drift that
  closed the window.
* Diagnostics available: `OTOMEDIUS_LOG=1|2|3` (loader/imports → per-call →
  per-dispatch), `OTOMEDIUS_NOMOVIE=1` (force `VFW_E_NOT_FOUND`),
  `OTOMEDIUS_NOTHREDS=1`, SIGUSR1/SIGTERM dumps the ring buffers,
  `x86_runtime` prints "STACK IMBALANCE" and "IMBALANCE ret at …" at level 2
  (noisy: shared epilogues and tail calls produce false positives).

---

## 5. Current state and active blocker

### What works (verified locally and in the Flatpak)
```
loader: image 00400000-005e9000 entry 00530e2b
imports: 300 bound, 0 missing
main: entering 00530e2b
user32: CreateWindowExA(1280x1024) wndproc=00401bb0
avs: ac_io_avs_boot
d3d9: Direct3DCreate9(32) -> 60000050
d3d9: GetDeviceCaps(this=60000050 adapter=0 type=1 caps=009fedc8)
d3d9: CreateDevice(1024x768) windowed=0
kernel32: CreateFileA(././HidBoot.id) -> fd 3
kernel32: CreateFileA(./oto.ini) -> fd 3
dsound: DirectSoundCreate8 -> 600403e0
kernel32: CreateFileA(././Data/otomedius.xgs) -> fd 3
kernel32: MapViewOfFile(116 bytes) -> 0a000000
dshow: filter graph created (600e0600)
dshow: AddFilter(10502664)
dshow: JoinFilterGraph -> 00000000
dshow: AddSourceFilter -> filter 600e0670
dshow: Render: subtype 1 -> ReceiveConnection 8004022a
dshow: CreateTexture(7f001540) -> 00000000 texture 600e0920
dshow: filter 10502664 method 5 -> 00522580 gives 00000000   (Pause)
dshow: filter event 1 (p1=00000000 p2=10502664)                (EC_COMPLETE)
dshow: filter 10502664 method 6 -> 005226e0 gives 00000000   (Run)
dshow: filter 10502664 method 4 -> 00522480 gives 00000000   (Stop)
```
* Window opens (1280×1024, **black**), `Esc` and the window close button quit
  correctly (both via `WM_QUIT`).
* The game runs its message loop indefinitely: `PeekMessageA` + `timeGetTime`
  + frame update; in 8 s of headless run it renders **~30 856 frames**
  (`BeginScene` idx 41, `EndScene` idx 42, `Present` idx 17, `Clear` idx 43).
* No crashes in the last build (`timeout` kills it, exit 124).

### Active blocker
**No `.pak` is ever opened**, so nothing is loaded and the screen stays black.
The game sits in the Konami boot state machine (`0x40C7F0`) and polls the touch
panel ~10 000 times/s (much faster than its own `Sleep(16)`, which suggests the
loop is being re-entered or the polling happens elsewhere as well).

Boot machine flow (reverse engineered):
```
state 0 (0x40C85C): bms_init(1,0x5be2c4,0x5bca6c,0x5be2ec)
                     0 → state 1 ; 2 → sound(0x15), state 3 ; 6 → sound(0x14), state 3
                     else → sound(0x19), state 3
state 1 (0x40C8E1): bms_init_wait()   1 → stay ; 0 → bms_eventlog_request_init()
                     0 → state 2 ; ≠0 → 0x40C90D
state 2 (0x40C9C7): bms_is_active()   0 → state 0 (loop!) ; ≠0 → return al = 2 (ready)
state 3 (0x40C9EB): return al = 1
state 4 (0x40C9F3): return al = 4
```
The caller `0x42D3D0` (site `0x42D5A0`) only continues when the machine returns
`2` or `4`; anything else (3) goes to an error path, and `1` keeps the caller in
its own wait loop.

With the current AVS answers the machine *should* reach state 2 and return 2,
but the assets are still not loaded, so either:
(a) the machine is being re-entered (keepalive) and something else gates the
    next step, or
(b) a later call in `0x42D3D0` after `0x42D5CE` (`0x402E90`, `0x402760`,
    `0x402FB0`, `0x402970`, `0x402840`) fails and the game falls back.
The functions that actually load the first scenes are known and **never
called**: `0x45EB20` (`Title.pak`) and `0x45FF10` (`Warning.pak`) — both are
virtual methods (`this` in `ecx`, no direct callers), i.e. the scene manager
never instantiates those scenes.

---

## 6. Immediate action plan (next 3 steps)

1. **Instrument the boot machine.** Add a one-shot log (level ≥ 2) in the AVS
   stubs or via a generated hook at `0x40C7F0`'s return sites, printing
   `state` (`[0x5E1F96]`), the return value, and the results of
   `bms_init`/`bms_init_wait`/`bms_eventlog_request_init`/`bms_is_active`.
   Confirm whether the machine returns `2` and how often it is re-entered.
   *Quick variant:* temporarily make `bms_is_active` return `1` and log every
   entry of `0x40C7F0` with `esp` (dispatch hook) to count re-entries.

2. **Check the post-boot path in `0x42D3D0`** (`0x42D5CE` → `0x402E90`,
   `0x402760`, `0x402FB0`, `0x402970`, `0x402840`): these functions take no
   arguments and return `al`; find which one returns failure (they print via
   `0x403DB0`/`0x402740` with the format strings at `0x5DB340`/`0x5596xx`).
   The first failing one is the next blocker.

3. **Find who instantiates the scenes** (`0x45EB20` Title, `0x45FF10` Warning,
   `0x439A8C` KonamiLogo): locate their vtable(s) in `.rdata` (the object
   constructors set a vtable pointer in `[esi]`), find the scene-manager
   switch that creates them, and hook it to see which scene is current. Once a
   scene loads, `.pak` files will start opening via `kernel32!CreateFileA`,
   which is the milestone to verify (`grep 'Data/.*\.pak'` in the log).

After that: the Linux renderer (DDS textures, `Clear`, then the sprite path
`0x40F780`/`0x40FA44`: log the vertex data at `DrawPrimitive`/`DrawIndexedPrimitive`
to reconstruct the 2D pipeline) and audio mixing (`dsound` → PulseAudio).

---

### Useful commands
```sh
# Flatpak build + run
bash build-game-flatpak.sh /path/to/otomedius.zip
flatpak run --env=OTOMEDIUS_LOG=1 io.github.nicobm.OtomediusRecomp

# local headless iteration (agent environment)
cd "<SALA DE TRABAJO>/.work"
zig/zig cc -std=c11 -O1 -w -I../otomedius-recomp/native -c ../otomedius-recomp/native/X.c -o obj/X.o
gen: ../otomedius-recomp/tools/recomp/recomp.py --exe oto/otomedius.exe --out gen --chunk 60
     PYTHONPATH=<pylib-with-capstone/pefile>
run: cd oto && timeout 20 env OTOMEDIUS_LOG=1 ../obj/otomedius-recomp .
```
