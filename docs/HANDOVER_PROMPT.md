# Otomedius (Arcade) — Handover para sesión nueva

> Copiar y pegar este documento entero como prompt inicial. El documento
> canónico y exhaustivo es `docs/HANDOVER.md`; **§10 manda sobre §1–§9**.
> Repo: `/var/home/vm/Proyectos/SALA DE TRABAJO/otomedius-recomp`.
> Estado del árbol: HEAD `f438662` + el commit de esta sesión (comprobar con
> `git log -1`). No hagas commits ni push sin pedirlo. **Nunca** escribas
> tokens en ficheros: el push va con `GIT_ASKPASS` + variable de entorno.
> Este repo **no contiene datos del juego**; el dump se traduce en la máquina
> del usuario.

---

## 1. Entorno y Toolchain

### 1.1 Target

| Item | Valor |
|---|---|
| Host / target | Linux **x86-64 only** (sin Windows/macOS/32-bit) |
| Entrega | Flatpak, runtime `org.freedesktop.Platform//26.08`, SDK `org.freedesktop.Sdk//26.08` |
| Pantalla | Wayland nativo (`--socket=wayland`) + XWayland (`--socket=fallback-x11`) |
| Sandbox | `--socket=pulseaudio --device=dri --device=input --nofilesystem=home --nofilesystem=host --unshare=network` (sin `--share=ipc`) |
| UI/GPU | **SDL3 3.4.20** (módulo Flatpak, sha256 `f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f`), renderer **OpenGL** |
| Recompilador | Python 3 + **capstone 5.0.9** (wheel en `flatpak/wheels/`, gitignored) |
| FreeType | 2.13.2 (headers/libs extraídos en `.work/freetype_include`, `.work/freetype_lib` para el contenedor del agente) |

### 1.2 Compilador y flags

**No hay CMake ni MSVC.** Un solo manifiesto (`io.github.nicobm.OtomediusRecomp.json`) ejecuta un script.

Flatpak (SDK `cc` = GCC):
```sh
cc -O2 -std=c11 -w -Inative $(pkg-config --cflags sdl3) \
   $(pkg-config --cflags freetype2) -c native/foo.c -o out/obj/foo.o
cc -O2 -std=c11 -w -Inative -c recomp/gen/rc_000.c -o out/obj/rc_000.o
cc -O2 -o out/otomedius-recomp out/obj/*.o -lm -lpthread \
   $(pkg-config --libs sdl3) $(pkg-config --libs freetype2)
```

Local del agente (**zig 0.17.1**, `.work/zig/zig`) — script persistente:
```sh
sh /var/home/vm/Proyectos/SALA DE TRABAJO/.work/tools/build.sh <archivo.c...>
# compila los .c indicados (native/ o tests/ por nombre) y re-linkea:
#   zig cc -O2 -std=gnu11 -w -Inative -I.work/sdl3/include \
#       -I.work/freetype_include -c ... -o .work/obj/X.o
#   zig cc -O2 -o .work/obj/otomedius-recomp .work/obj/*.o \
#       .work/obj/gen/*.o -lm -lpthread -L.work/freetype_lib -lfreetype \
#       -L$REPO/build-dir/files/lib -lSDL3
```
> **Ojo:** el `build.sh` actual enlaza SDL3 desde `otomedius-recomp/build-dir/files/lib`
> (se añadió esta sesión; hay copia en `build.sh.bak`). `pkg-config` no existe
> en el contenedor del agente.

**REGLA CRÍTICA:** `cpu_t` está en `native/x86rt.h` y se incrusta en los 85
`rc_*.c`. **Cualquier cambio en ese header exige regenerar y recompilar TODO**
(incluidos los generados). Mezclar objetos viejos dio corrupción silenciosa.

### 1.3 Pipeline de recompilación

```sh
# regenerar C desde el exe (seeds en recomp/seeds.txt)
cd /var/home/vm/Proyectos/SALA DE TRABAJO/otomedius-recomp
PYTHONPATH=../.work/pylib python3 tools/recomp/recomp.py \
    --exe ../.work/oto/otomedius.exe --out ../.work/gen --chunk 60
# ejecutar
cd ../.work/oto && ../obj/otomedius-recomp .
```

Resultado del recompilador: **5 022 funciones, 381 616 instrucciones, 95,7 %
de `.text`, 78 955 entradas de dispatch, 85 ficheros `rc_000.c`…`rc_084.c`**.
`recomp/seeds.txt` tiene 7 entradas manuales:
`419700 40f4d0 40f570 40f5c0 40f5f0 419730 419740`.
Modelo: una función C por función x86, `goto` intra-función, `cpu->eip =
target; return;` para saltos entre funciones, dispatcher `x86_entries[]`
(binary search) y `cpu->cont` para reanudar. Sólo se despachan entradas
(`cont == 0`) y sitios de retorno de `call`.

### 1.4 Estructura de directorios

```
otomedius-recomp/
├── README.md  FLATPAK.md  LICENSE  VERSION
├── build-game-flatpak.sh                  # único script de usuario
├── io.github.nicobm.OtomediusRecomp.json  # manifiesto Flatpak
├── flatpak/                               # launcher, .desktop, .svg, metainfo, wheels/
├── native/                                # capa Linux (18 ficheros, ~8k líneas)
│   ├── main.c            boot, fault handler, watchdog, x86_thread_begin/exit
│   ├── memory.c          mmap 4 GiB, carga PE, TEB/PEB
│   ├── imports.c         imports PE, thunks nativos (tabla fija 0x10000×24 B)
│   ├── patches.c         overrides por dirección + BOOTDBG
│   ├── config.c          oto.ini (tabla 0x5BE328) + init del ring
│   ├── x86rt.h/.c        cpu_t, dispatcher, ALU/x87/SSE2, watch/watchmem
│   ├── win32.h           helpers (ARG32, RET32, M32, log_printf…)
│   ├── win32_kernel32.c  ficheros, heaps, VM, threads, TLS, code pages, time
│   ├── win32_user32.c    ventana + cola de mensajes
│   ├── win32_misc.c      gdi32 (GetGlyphOutlineA), ole32/oleaut32/shlwapi/imm32/winmm
│   ├── d3d9.h/.c         IDirect3D9 (17) + IDirect3DDevice9 (119), D3DCAPS9
│   ├── d3d9_objects.c    texture(22)/surface(17)/buffer(14)/shader(5) + DXT
│   ├── d3dx9.c           matemáticas, DDS, D3DX stubs
│   ├── dsound.c          IDirectSound8(12) + IDirectSoundBuffer8(24) + MEZCLADOR SDL
│   ├── dshow.c           grafo DirectShow falso (TextureRenderer del juego)
│   ├── movie.c/.h        decodificador vídeo+audio con ffmpeg
│   ├── avs.c             libacio/libbms/libtouchpanel + input
│   ├── render.c          rasterizador software, estados D3D, dumps, stats
│   ├── font.c/.h         backend GDI FreeType
│   └── platform.h/_sdl.c ventana SDL3, eventos, input, framebuffer, audio
├── tools/recomp/         pe.py, xdis.py, emit.py, recomp.py
├── recomp/exe.sha256  recomp/seeds.txt
├── tests/platform_null.c backend headless
└── docs/HANDOVER.md  docs/HANDOVER_PROMPT.md
```

Artefactos locales fuera del repo: `.work/gen` (C generado), `.work/obj`
(nativos + `obj/gen`), `.work/oto` (dump extraído), `.work/pylib` (capstone +
pefile), `.work/sdl3/include`, `.work/freetype_*`, `.work/tools/`.

### 1.5 Diagnóstico y ergonomía (implementado, conservar)

```sh
OTOMEDIUS_LOG=0|1|2|3
OTOMEDIUS_WATCH="addr[:n],…"        OTOMEDIUS_WATCHMEM="addr,…"
OTOMEDIUS_PEEKREGS=1                OTOMEDIUS_PEEKARGS=1
OTOMEDIUS_WATCHCHG=auto|<addr>
OTOMEDIUS_DUMP_FB=file.ppm          OTOMEDIUS_DUMP_AT=n
OTOMEDIUS_DUMP_EVERY=n              # file.NNNNN desde DUMP_AT
OTOMEDIUS_DRAWDBG=n|1               # draws del present n; con cualquier valor
                                    # imprime SKIP/DISCARD + matrices
OTOMEDIUS_TEXFMT=1                  # formato/tamaño de cada DDS
OTOMEDIUS_BOOTDBG=1                 # boot/attract + loader + DEMO state
OTOMEDIUS_DSDBG=1                   # DirectSound: Create/Play/Stop/vol/freq/Lock
OTOMEDIUS_AUDIO_DUMP=file.raw       # mezcla dsound (S16LE 48k estéreo)
OTOMEDIUS_AUDIO_STREAM_DUMP=file.raw# stream final SDL (vídeo + juego)
OTOMEDIUS_WINDOWED=1                # ventana en vez de fullscreen
OTOMEDIUS_WINDOW_SIZE=WxH           # p.ej. 800x450 (16:9); default 1366x768
OTOMEDIUS_QUIT_AT=n                 # salida limpia en el present n
OTOMEDIUS_TEST_QUAD=1|2  OTOMEDIUS_TEST_TEX=0xADDR
OTOMEDIUS_TEXTURES=0|1  OTOMEDIUS_CHKSUM=1  OTOMEDIUS_LOADER_GUARD=1
OTOMEDIUS_NOMOVIE=1  OTOMEDIUS_MOVIE=1  OTOMEDIUS_MOVIE_PREVIEW=1
OTOMEDIUS_NATIVE_MOVIE=1  OTOMEDIUS_MOVIE_FILE=path  OTOMEDIUS_MOVIE_SIZE=WxH
OTOMEDIUS_BEZEL=path|0  OTOMEDIUS_BEZEL_DUMP=file.ppm  OTOMEDIUS_FULLSCREEN=1
OTOMEDIUS_FONT=path.ttf  OTOMEDIUS_NO_FREETYPE=1  OTOMEDIUS_TEXDBG=1
OTOMEDIUS_AUTOCOIN=1  OTOMEDIUS_AUTOSTART=1  OTOMEDIUS_BADTARGET=skip
OTOMEDIUS_WATCHDOG=1  OTOMEDIUS_NOTHREDS=1  OTOMEDIUS_KONAMI=1
OTOMEDIUS_FORCE_JOB=1  OTOMEDIUS_DIR=path
```

**Pruebas:** usar ventana pequeña 16:9, **no fullscreen**:
```sh
cd /var/home/vm/Proyectos/SALA DE TRABAJO/.work/oto
OTOMEDIUS_WINDOWED=1 OTOMEDIUS_WINDOW_SIZE=800x450 \
  OTOMEDIUS_DUMP_FB=/tmp/opencode/x OTOMEDIUS_DUMP_AT=3400 \
  OTOMEDIUS_QUIT_AT=3402 timeout -k 5 900 ../obj/otomedius-recomp .
```
Corridas automáticas largas sin ventana: `SDL_VIDEODRIVER=offscreen`.
Al pulsar **Esc** se registra `platform: Esc pressed` y se escribe
`<DUMP_FB>.quit` con el frame exacto (`dump_on_exit()`); el log completo de
la corrida queda en el fichero de redirección.

**Ghidra MCP** (activo): proyecto `gghidra`, programa `/otomedius.exe`,
4 533 funciones. Desde `execute`:
`tools["ghidra-mcp"].get_functions({functions:"0x…", fields:"entry_point,decompiled_code"})`
o `force_decompile({address:"0x…"})`; también HTTP `127.0.0.1:8089`.

---

## 2. Mapa de Binarios y Memoria

### 2.1 Dump y ejecutable

| | |
|---|---|
| Dump | `Otomedius (2008-04-18)(JPN)[Konami PC][TP].zip`, sha256 `4c6562eab3fff1bd1a6e84732bcb565a871440fcb894985fe990fe566a8bd897` (~452 MB) |
| Contenido | `otomedius.exe`, `otomedius_edit.exe`, `bms_GGG.exe`, `d3dx9_32.dll`, `libacio.dll`, `libavs-win32.dll`, `libbms.dll`, `libtouchpanel.dll`, `dbghelp.dll`, `HIdGet.exe`, `HidBoot.id`, `ChkSum.bin`, `bookkeep.bin`, `ea_conf.bin`, `testmode.bin`, `card_allow.list`, `otomedius.ini`, `Data/*.pak` (151, 436 MB, CAB `MSCF`), `Data/*.xwb`, `Data/OTOMEDIUS_OP_f.wmv`, `Data/*.dds` |
| Exe | PE32 i386 GUI, 1 945 600 B, sha256 `c55796ad1f2d6ea9bd0fb0d33ed6b6eb6463d0e3ad8f2e1d7c4608ca9dfcf112` |
| Image base / entry | `0x400000` / `0x00530E2B` |
| Secciones | `.text 0x1000 sz 0x14B6B7` · `.rdata 0x14D000 sz 0x452E0` · `.data 0x193000 sz 0x54FA8` · `.rsrc 0x1E8000` |
| PDB / compilador | `c:\Local\proj\otome\soft\src\prog\Release\moedius.pdb`, MSVC 2005 Release, x87 (sin SSE en el juego), SEH `fs:[0]` |
| Imports | 300 bound, 0 missing |

**`.data` sólo está parcialmente respaldada por fichero**: desde ~`0x5D0000`
carga como cero (BSS). Ahí viven el ring del loader, heaps locales, registro
de paquetes y globals de config. `pe.read(va-0x400000)` da
`PEError: not backed by file data`.

DLLs importadas: `d3d9`(1), `d3dx9_32`(34), `imm32`(1), `winmm`(5),
`libacio`(32), `libtouchpanel`(13), `libbms`(66), `dsound`(ordinal **11** =
`DirectSoundCreate8`), `kernel32`(104), `user32`(27), `gdi32`(7), `ole32`(6),
`oleaut32`(ordinal 2/6), `shlwapi`(1).

### 2.2 Espacio de direcciones emulado

| Rango | Uso |
|---|---|
| `0x00000000`–`0xFFFFFFFF` | `mmap(MAP_NORESERVE)` 4 GiB; `MEM` |
| `0x00400000`–`0x005E9000` | imagen PE en su base original |
| `0x000A0000`+ | CLI, campos PEB |
| `0x02000000`+ | stacks por hilo (1 MiB; slot N top = `0x02000000+(N+1)*1MiB`) |
| `0x0A000000`–`0x20000000` | `MapViewOfFile` (bump); `Once.pak` en `0x0A001000` |
| `0x10000000`–`0x30000000` | `HeapAlloc` (`hdr_t{size,used,heap,magic=0x4F544D48}`, first-fit) |
| `0x30000000`–`0x60000000` | `VirtualAlloc` (bump) |
| `0x60000000`+ | objetos COM + `emu_alloc` (texturas, buffers); llegó a `0x66BAE320` |
| `0x7F000000` | thunks nativos, **16 B de paso**, `idx=(addr-0x7F000000)/16` |
| `0x7FF00000`+ | TEB por hilo (0x2000); main en `0x7FFDE000`; PEB `0x7FFDF000` |

`X86_RET_SENTINEL = 0xFFFFFFF1`. `HANDLE_BASE=0x1000`, 4 B/handle,
`MAX_HANDLES=4096`.

### 2.3 Globals verificadas en runtime

| Dirección | Significado |
|---|---|
| `0x005E1F1C` | segundo `IDirect3D9*` |
| `0x005E1F20` | **`IDirect3DDevice9*`** |
| `0x005E1F24` / `0x005E1F26` / `0x005E1F27` | gate de frame / pacing / rendering enabled |
| `0x005E1F28` | descriptor del heap local (`[+0]` nombre `0x55614C`, `[+4]` primer bloque) |
| `0x005E1F50` | HWND |
| `0x005E1F7C` | objeto log (`+0x51` buffer 255 B) |
| `0x005E1F88` / `0x005E1F8C` | `CSound*` / sonido secundario |
| `0x005E1F96` | índice del boot state machine (0..4; estable en 2) |
| `0x005E1FA5` / `0x005E1FA6` / `0x005E1FA7` | I/O board 5=PJEI / touch 3 / card >1 |
| `0x005E1FFD` / `0x005E1FFE` | I/O started / `ac_io_update` |
| `0x005E2000` | handle del touch panel |
| `0x005E2024` | índice del replay del attract (0..3) |
| `0x005E2027` | asset request: 0 load, 1 Once.pak, 2 wait, **0xFF done** |
| `0x005BB434` | one-shot de creación de escena |
| `0x005BC914` | handle del worker del loader |
| `0x005BC918` | id del job actual |
| `0x005BC91C` | estado: 0 idle, 1 running, 2 done, **3 CANCELADO**, 4 error |
| `0x005B3F82` | flag del hilo loader (0 entrada, 1 salida) |
| `0x005C324C`…`0x005C3258` | 4 args del finalise `0x414920` |
| `0x005DCF8C` | id "actual" del ring |
| `0x005DCF90` / `0x005DCF94` | head / tail (iguales = vacío) |
| `0x005DCF98` | ring del loader: 16 slots × `0x10C`; `[+4]` status, **-1 = libre** |
| `0x005DC9A2` | loader activo |
| `0x005DC98B` / `0x005DC9BC` | switch byte / switch word |
| `0x005DCF70` | salida del finalise |
| `0x005DE058` / `0x005DE070` | CRITICAL_SECTIONs |
| `0x005DE098 + id*4` | tabla de **16** paquetes; `0x5DE0BC` = id 9 (logos/warning/title/attract) |
| `0x005DE0F0 + 4i` | 14 voces de sonido × `0x48` B (`+8` buffer, `+0x10` record, `+0x14` flags, `+0x34` contador) |
| `0x005DE128` / `0x005DE12C` | id / flag del hilo de sonido |
| `0x005DE220` | máscara de managers (`0xFFFFFFFF` = suspendidos) |
| `0x005DE224` | color de fade compartido |
| `0x005DE228`…`0x005DE27C` | slots de managers; `0x5DE24C` boot/attract, `0x5DE258` manager exclusivo, `0x5DE25C` EyeCatch, `0x5DE260` CGameMan |
| `0x005DB130`–`0x005DB278` | config (`memset(0x5DB130,0,0x148)` en `0x401EC6`) |
| `0x005DB150` | `wSortDrawCmn2DMax` (u16, **256** o error 1115) |
| `0x005DB454` / `0x005DB458` / `0x005DB45C` | `640.0` / `512.0` / `1.0` (centro 2D) |
| `0x005DB708` / `0x005DB710` | magic `0x07091300` / offset de reloj s64 de `testmode.bin` (en este dump `-1 300 000 ms`) |
| `0x005BE328` | tabla ini `{name,target,type}` ×97 (1 byte, 3 u16, 6 u32, 7 float) |
| `0x005BE388` | entrada `wSortDrawCmn2DMax` |
| `0x005E6E60` / `0x005E6E28` | flags SSE2/runtime |

Constantes: `D3DFMT_X8R8G8B8=0x16`; `D3DERR_DEVICELOST=0x88760868`;
`D3DERR_DEVICENOTRESET=0x88760869`; `VFW_E_TYPE_NOT_ACCEPTED=0x8004022A`;
`VFW_E_NOT_FOUND=0x80040216`; `E_NOINTERFACE=0x80004002`;
`D3DTA_*`/`D3DTOP_*` según d3d9.h.

### 2.4 Formatos DDS reales (esta sesión)

| Textura | Tamaño | `pf_flags` | bits | Máscaras | Formato |
|---|---|---|---|---|---|
| `Title.pak`/`main_visual.dds` | 1024×1024 | `0x40` | **24** | `ff0000/ff00/ff/0` | **R8G8B8** |
| `Title.pak`/`t_logo.dds` | 2048×512 | `0x41` | 32 | `ff0000/ff00/ff/ff000000` | A8R8G8B8 |
| `KonamiLogo.pak`/`konami_logo.dds` | 1024×256 | `0x40` | 16 | `f800/7e0/1f/0` | **R5G6B5** |
| `Player02.pak` | 6 texturas | `0x41` | 16 | `7c00/3e0/1f/8000` | A1R5G5B5 |
| `catch_demo02.pak` | 2048×1024 | `0x40` | **24** | `ff0000/ff00/ff/0` | **R8G8B8** |
| `SysFont.dds` | 128×128 | — | 16 | `f00/f0/f/f000` | A4R4G4B4 |
| `gradius_font.dds` | 256×256 | `0x4` | — | `DXT5` | DXT5 |
| `Warning.pak` | 1024×1024 | `0x40` | 16 | `f800/7e0/1f/0` | R5G6B5 |

---

## 3. Funciones Recompiladas y Subsystems Resueltos

### 3.1 Recompilador (`tools/recomp`)

Capstone 5.0.9, parser PE propio (`pe.py`), discovery por seeds → fixpoint de
calls → punteros inmediatos → heurística de prólogo → fixpoint de branches →
adopción de palabras de datos. Jump tables `jmp dword ptr [reg*4+tabla]`
leídas estrictamente. 5 022 funciones / 381 616 instrucciones / 85 ficheros.
Mnemónicos sin manejar (inalcanzables hasta ahora): `int3`(59), `in`(14),
`out`(7), `les`(6), `aaa`(5), `hlt`(4), `jecxz`(2), `aam`(2), `lcall`(2),
`cmpltpd`(2), `cmpeqsd`(2), `int`(1).

### 3.2 Tabla de funciones clave (direcciones originales)

| Dirección | Rol |
|---|---|
| `0x00530E2B` | `_WinMainCRTStartup` |
| `0x0053B2AD` | `__tmainCRTStartup` |
| `0x00530547` | wrapper de hilo CRT (entrada real `[param+0x54]`, arg `[param+0x58]`) |
| `0x0051BA88` | thunk `imm32!ImmDisableIME` |
| `0x0051BA10` | thunk `d3dx9_32!D3DXCreateTexture` |
| `0x0052DB7A` | thunk `dsound!DirectSoundCreate8` |
| `0x00401BB0` | **wndproc**; jump table `0x401D74`, índice `msg-2`; WM_ACTIVATE→`0x401BE1`, WM_DESTROY→`0x401BF2` |
| `0x00401100` | CRT WinMain caller |
| `0x00401EC6` | `memset(0x5DB130,0,0x148)` |
| `0x00402230` | shutdown (`0x439FB0`, `CSound::Move` `0x4050B0`, `0x418190`, `0x41A9A0`) |
| `0x004024C0` | frame update (`TestCooperativeLevel`, `0x401A00`, `0x412FE0`, `0x439FE0`, `0x41AA10`, `0x402340`) |
| `0x00402340` | render (`0x40F780`, EndScene 42, Present 17) |
| `0x004025C0` | message loop (`PeekMessageA`, `0x4024C0`, `DestroyWindow`, `timeGetTime`) |
| `0x00402E90` / `0x00402760` | lazy init LoadConfig `0x402F10` / LoadBookKeep `0x4027A0` |
| `0x00402F10` | LoadConfig `E:\nvmem\testmode.bin`, size 0x48, magic `[0x5DB708]==0x07091300` |
| `0x004027A0` | LoadBookKeep `E:\nvmem\bookkeep.bin`, size 0x290, magic `[0x5DB478]==0x20070404` |
| `0x0040EC30` / `0x0040EFB0` | read-whole-file |
| `0x0040ED00` | integrity check MD5 de una entrada de `ChkSum.bin` (parcheado) |
| `0x00403DB0` | log con id de 4 dígitos; **pone `[0x5DE220]=0xFFFFFFFF`** |
| `0x00405120` | sink de eventos/log (`0x405120(id,arg)`), no `CSound::Init` |
| `0x0040C7F0` | boot state machine Konami (`Sleep(16)`, touch, io, jump table `0x40C9FC`) |
| `0x0042D3D0` | **scene/asset manager** (thiscall, `ecx=this`, slot `0x5DE228`); gates `0x5E1FA5==5`, `0x5E1FA6==3`, `0x5E1FA7>1`; sub-estados `0x42DBA4` |
| `0x0042D9F8` | creación de escena (`HeapAlloc` `0x3C`→`0x45F1B0`, `0x18`→`0x439C80`, `0x420`→`0x44D190`) |
| `0x0042DC30` | handler de escena/mensajes |
| `0x0043A030` | manager loop (`0x439FE0`): `[obj+4]&[0x5DE220]` skip; `call [vtbl+8]`; `al==0`→`call [vtbl]` y limpia slot |
| `0x00412EA0` | I/O init (`ac_io_begin`, ventana, touch, calibrate, `[0x5E1FFD]=1`) |
| `0x00412FE0` | I/O update por frame |
| `0x004132C0` | detección de cabina (jump table `0x413BD0`, touch `0x413BEC`, card `0x413C0C`) |
| `0x00413CD0` | misc por frame (touch poll, `ac_io_update`) |
| `0x004142B0` | **heap local del juego** (alloc) |
| `0x00414460` | free del juego |
| `0x00414920` | finalise/commit (`edi=0x419690(0)`, 4×`call [[edi]+0x24]`) |
| `0x00419690` | registry getter (`[0x5DE098+4*id]`, vector `+0x24/+0x28`, elem 0x38) |
| `0x00418040` | **load-job executor** (`0x419E30` gate, `0x40EE90` abre/mapea, `0x4185B0` CAB, `0x4186F0` grupo) |
| `0x00418730`… | creación de paquete (`EnterCriticalSection 0x5DE058`, `HeapAlloc 0x44`, `0x419110`) |
| `0x00419110` | ctor de paquete |
| `0x00419330` / `0x004193A0` | `Cancel(id)`: si el job actual tiene ese id → estado 3; marca `-1` en slots encolados |
| `0x00419D00` | cuerpo del worker del loader |
| `0x00419530` | poll "¿terminó?" (`Sleep(16)` + `0x419430`; 1 si pump∈{0,2}) |
| `0x00419430` | pump del loader (main thread) |
| `0x00419CA0` | lanza worker (`_beginthreadex(0x419D00)`, prioridad -2, `[0x5BC91C]=1`) |
| `0x00419E30` | "¿job cancelado?" → 1 iff `[0x5BC914]!=0 && [0x5BC91C]==3` |
| `0x0041A820` | `CDirectSound::Init` |
| `0x0041B2A0` | **hilo de sonido**: 14 voces, `GetCurrentPosition`, `Stop` al pasar la longitud |
| `0x0041BE20` | duplica buffer por voz (`DuplicateSoundBuffer`) |
| `0x00404FB0` / `0x00405120` | `CSound::Create` / `Init` |
| `0x0045F1B0` / `0x00439C80` / `0x0044D190` | constructores de escena/managers |
| `0x0045EB20` | **Title scene** (`.\Data\Title.pak`, `0x567094`) |
| `0x0045FF10` | **Warning scene** (`.\Data\Warning.pak`, `0x5671EC`) |
| `0x00439A8C` | referencia `.\Data\KonamiLogo.pak` (`0x55C884`) |
| `0x004399A0` | ctor manager KonamiLogo (vtable `0x55C8A0`, registra `0x5DE258`) |
| `0x00439A70` | update logo (estado `[obj+0xA]` 0..4, tabla `0x439B7C`); estado 0 pide `KonamiLogo.pak` id 9 |
| `0x00439B90` → `0x00439BB0` | draw logo: textura = `grupo[0]+0` de `pkg9` (`[0x5DE0BC]`) |
| `0x00439A10` | dtor del logo; `0x418190(9)` |
| `0x0043F280` | ctor manager de vídeo (vtable `0x55E904`, registra `0x5DE258`) |
| `0x0043F370` | update vídeo (estados 0..4, timer 3570) |
| `0x0043F4E0` | draw vídeo (encola `0x43F540`) |
| `0x0043F540` | callback del quad de vídeo (x87 degenerado; ver §4) |
| `0x00417480` | sonda DirectShow (abre/cierra grafo) |
| `0x004174B0` | abre movie wrapper (`0x1C` B en `[0x5DCF88]`) |
| `0x00417A80` | construye grafo (`CoCreateInstance`, `AddFilter`, `AddSourceFilter`, `Render`) |
| `0x00417D90` / `0x00417590` | cierra wrapper |
| `0x004175C0` | ctor `TextureRenderer` (IBaseFilter `+0xC`, vtables `0x55926C/0x559334/0x559374/0x55938C`) |
| `0x004176E0` | `SetMediaType`: exige `FORMAT_VideoInfo`, `MEDIATYPE_Video`, `MEDIASUBTYPE_RGB24` (`GUID 0x55917C`) |
| `0x00417750` | crea textura de vídeo 1280×1024 X8R8G8B8 |
| `0x00527110` / `0x00521390` / `0x00521400` | pin methods del renderer |
| `0x0052DA30` | copia media type con `CoTaskMemAlloc` |
| `0x004547E0` | **boot/attract manager**; tablas `0x45598C` (estados 0..5) y `0x4559A8` (sub-estados) |
| `0x00429210` | **ctor `CDemoPlayMan`** (vtable `0x55AE88`, registra `0x5DE258`, `[+4]=0x1000`, estado `[+0xA]=0`) |
| `0x00429340` | **update `CDemoPlayMan`** (estado 0..5; ver §5) |
| `0x004294C0` | draw `CDemoPlayMan` (encola callback `0x429520` en `[0x5E1F9C]`) |
| `0x0042CE60` | ctor `CEyeCatchMan` (slot `0x5DE25C`, `[+4]=0x2000`, `[+0xE]/[+0xF]=0`) |
| `0x0042E1F0` | ctor `CGameMan` (slot `0x5DE260`, `[+4]=0x4000`; exige paks PLAYER/POWERUP/STAGE) |
| `0x0040CAF0` / `0x0052DF0D` / `0x0052FC65` | reloj→cadena / helpers CRT time64 (rechazan negativos) |

### 3.3 Subsystems nativos implementados

* **kernel32** – ficheros (disposición Win32: `OPEN_EXISTING=3→O_RDWR`,
  `OPEN_ALWAYS=4→O_CREAT|O_RDWR`), heaps first-fit, `VirtualAlloc`, threads
  con stack+TEB, TLS, code pages `iconv` (CP932/936/949/950/1252),
  `GetProcAddress`, mappings, time, critical sections, events/semaphores.
  `win_host_path()` quita la letra de unidad y el componente `nvmem\`.
* **user32** – una ventana (`HWND 0x00010001`), cola, `RegisterClassExA`
  (`lpfnWndProc` en +8), `CreateWindowExA` envía WM_CREATE/SIZE/ACTIVATE/
  SETFOCUS; Esc/close → WM_QUIT.
* **d3d9** – `IDirect3D9` (17) + `IDirect3DDevice9` (119) con vtables de
  thunks nativos; `SetFVF`, `DrawPrimitiveUP`/`DrawPrimitive` con conteos
  correctos; caps realistas; estados RS/TSS/sampler; transformaciones
  WORLD/VIEW/PROJ **guardadas** en `render.c` (para el futuro camino XYZ).
* **d3d9_objects** – texture(22)/surface(17)/buffer(14)/shader(5),
  `LockRect` real, `GetSurfaceLevel` comparte buffer, DXT1/3/5 + L8/A8 +
  A4R4G4B4 + A1R5G5B5 + R5G6B5 + **R8G8B8 + X8R8G8B8 + X1R5G5B5**,
  caché de 64 texturas (`texture_argb`).
* **d3dx9** – matemáticas reales (`MatrixMultiply/Translation/Scaling/
  Rotation*/LookAtLH/PerspectiveFovLH/Transpose/Inverse`, `Vec3*`,
  `QuaternionSlerp`, `PlaneTransform`, `Vec2Hermite`), `D3DXCreateTexture`,
  parse DDS (`decode_dds`), `D3DXCreateTextureFromFileExA` real,
  `D3DXLoadMeshHierarchyFromXInMemory` → `S_OK` con salidas NULL.
* **dsound** – `IDirectSound8`(12) + `IDirectSoundBuffer8`(24); buffers con
  memoria emulada, `Lock/Unlock/Play/Stop/volumen/pan/freq`, clones que
  comparten `data` y vtable, **mezclador a SDL** (ver §4).
* **dshow** – grafo falso completo: `IGraphBuilder`(18), `IBaseFilter`(15),
  `IPin`(19), `IMediaControl`(16), `IMediaEvent`(13), `IMediaEventEx`(16),
  `IVideoWindow`(46), `IBasicAudio`(11), `IMediaPosition`(18),
  `IMediaEventSink`(4); media type RGB24 1280×1024; `EC_COMPLETE`.
* **avs** – reemplazo de libacio/libbms/libtouchpanel (ver §4.1) + input.
* **render/platform** – framebuffer software 0xAARRGGBB, triángulos
  `XYZRHW|DIFFUSE|TEX1` (stride 28) con wrap, blend, alphatest, TSS,
  sampler, filtro POINT/LINEAR; stats cada 120 presents; dump PPM;
  `platform_sdl.c` sube el framebuffer a una textura streaming.
* **font** – FreeType 2.13.2: `CreateFontA` (`h/4` px), `GetTextMetricsA`
  (56 B), `GetCharABCWidthsA`, `GetGlyphOutlineA` **GGO_GRAY8_BITMAP** (6,
  cobertura 0..64), GGO_BITMAP (1), GGO_NATIVE (2); avance en `gm+0x10`.
* **movie** – `ffmpeg` para vídeo (WMV3 640×512 30 fps) y audio (WMAv2 →
  S16LE 48k estéreo); hilos con anillo de 3 frames; `-re` sólo en vídeo.

---

## 4. Parches y Workarounds Aplicados

### 4.1 Konami AVS (`native/avs.c`, todos cdecl `nargs=0`)

| Función | Devuelve | Motivo |
|---|---|---|
| `ac_io_avs_boot` | `0` | I/O booteado |
| `ac_io_begin` | **`1`** | el juego aborta si `al==0` |
| `ac_io_update` | `0` | |
| `ac_io_pjei_update_control_status_buffer` | **`1`** | con 0 el input se congela para siempre |
| `ac_io_pjei_get_control_status_buffer` | `0` + 16 B | switch word activo a nivel bajo: bit0 up,1 down,2 left,3 right,4..7 shot/bomb/option/special,8 start,9 coin |
| `ac_io_pjei_unlock_coincounter` | **`1`** | 0 → error 1183 → `[0x5DE220]=0xFFFFFFFF` |
| `bms_time_get_clock_blocking(type,uint64*)` | `0`, **CLOCK_REALTIME** ms | el puntero es `arg32(1)`; con monotónico el offset negativo de `testmode.bin` daba time64 negativo → *unhandled exception* |
| `ac_io_is_active2(dummy,&st)` | **`1`**, `*st=0` | 0 → modo I/O 7 |
| `ac_io_pjei_req_secplug_check()` | **`1`** | 0 → modo 7 |
| `ac_io_pjei_req_secplug_check_isfinished(&st)` | **`1`**, `*st=0x14` | `0x4133D6` compara con 20 |
| `ac_io_pjei_get_systemid/get_softwareid` | **`1`** + 20 B `"0123456789ABCDEFGHIJ"` | 0 → modo 7 |
| `ac_io_icca_cardunit_init()` | **`1`** | sale del card state 0 |
| `ac_io_icca_cardunit_init_isfinished(1,&s)` | **`1`**, `*s=4` | `0x41398C` acepta 4/5 |
| `ac_touchpanel_new` | `emu_alloc(64)` real | el juego lo desreferencia |
| `ac_touchpanel_send_check_diag` | **`1`** | |
| `ac_touchpanel_check_wait_receive` | `0` | |
| `ac_touchpanel_receive_check_diag(h,&st)` | **`1`**, `*st=1` | 0 → bucle |
| `ac_touchpanel_read_on[_direct]` | **`1`** + coords | 0 = panel no responde |
| `ac_touchpanel_caribrate_set` / `polling` / `renewal` | `0` | |
| `bms_init` / `bms_init_wait` / `bms_eventlog_request_init` | `0` | avanza boot |
| `bms_is_active` | **`1`** | 0 → boot state 0 en bucle |
| resto libacio/libbms/libtouchpanel | `0` | free play, sin red/card |

### 4.2 D3D9/D3DX

* `GetDeviceCaps`: caps realistas (SM 3.0 `0xFFFE0300`/`0xFFFF0300`,
  `MaxVertexShaderConst 256`, `MaxStreams 16`, `MaxPrimitiveCount 0x7FFFFF`,
  `MaxVertexIndex 0xFFFFFF`, `MaxTexture 8192`, `MaxSimultaneousTextures 8`,
  `DevCaps HWTRANSFORMANDLIGHT`). **`pCaps=arg32(3)`**.
* `GetAdapterIdentifier`→`arg32(3)`; `EnumAdapterModes`→`arg32(4)`;
  `CreateDevice` `ppDevice=arg32(6)`, `D3DPRESENT_PARAMETERS` (+0 W, +4 H,
  +32 Windowed).
* `dev_Present` → `render_present()`; `TestCooperativeLevel` → 0.
* `D3DXCreateEffect` → `E_FAIL` (el port ignora errores 2302/2306).
* `D3DXLoadMeshHierarchyFromXInMemory` escribe **NULL** en las dos salidas.
* `D3DXCreateTextureFromFileInMemoryEx`: `ppTexture=arg14`; **respeta el
  formato real del DDS** (esta sesión).
* **`decode_dds` por bits/máscaras** (esta sesión): R8G8B8 (24),
  X8R8G8B8, R5G6B5 (5-6-5, verde de 6 bits), A1R5G5B5, X1R5G5B5,
  A4R4G4B4, DXT1/3/5. `texture_argb` decodifica todos.
* **Bug del rasterizador corregido**: `draw_tri` conserva el **signo** del
  área (los quads del juego son clockwise); con `fabs` las UV salían
  negadas (texto mal y logos 180°).
* UV por celdas (texto): si `max(u,v)>=1.5` y el span es <1, expande la
  parte entera con el tamaño de celda del quad.

### 4.3 Loader / threads / paquetes

* `thread_arg_t` con **`calloc`** (antes `done` basura → `WaitForSingleObject`
  decía "terminado" y arrancaba dos workers).
* Guard de `0x419530` **opt-in** (`OTOMEDIUS_LOADER_GUARD=1`).
* Tabla de thunks **fija** `g_thunks[0x10000]` + mutex (el `realloc`
  corrompía al hilo del mezclador).
* Clones dsound comparten la **vtable** del original.
* `0x418040`: id de paquete `< 16` (tabla de 16), no `< 4`.
* `0x418190`: escaneo de records muertos; commit del **id 9 diferido**;
  `pkg+8=pkg+4` al entrar en `0x4186F0` sólo si el stream no es `SDBK`.
* `0x414920` (finalise): skip si el registro está vacío o el record no
  tiene vtable.
* `0x4142B0`/heap local: inicialización del ring con `-1`.
* `0x402840` (save bookkeep) skip: el path corrompe el nombre y lanza SEH.
* `0x4050B0` (shutdown de sonido) reimplementado tolerando objeto muerto.
* `ExitThread` longjmp al `jmp_buf` del hilo (no `pthread_exit`).
* `CloseHandle` de hilos deja zombis para que el juego siga sondeando.
* `teb_ok()` (rango `0x7FF00000..0x7FFFFFFF`) en TLS; `UnhandledExceptionFilter`
  y `fault_handler` imprimen código/dirección/contexto/última importación.

### 4.4 Boot / escena / vídeo

* **Sin forzados** en `0x4547E0` (los workarounds antiguos creaban el estado
  muerto `state=1 sub=8`).
* `0x403DB0`: ignora exactamente los errores de shader 2302/2306.
* `0x41C620`: coordenadas no finitas → 0 (el `fdiv` de `0x43F540` metía
  `+inf` en Y).
* `0x43F370`: en state 3 suma los ticks reales de 60 Hz (el intro dura ~60 s
  aunque el port vaya a 20–30 fps).
* `0x417480`/`mc_Run`: el decodificador de vídeo sólo arranca si el manager
  de vídeo es dueño de `0x5DE258` (vtable `0x55E904`); la sonda no pitaba.
* `mc_Stop` cierra el decodificador; Esc llama `movie_close()` antes de
  `win_request_quit()`.
* `0x4176E0`: media type **RGB24** (no RGB32/YUY2), 1280×1024, 30 Hz.
* `dshow` `gb_Render` compara HRESULT como `(int32_t)`; `fg_AddFilter`
  limpia `g_nfilters` en grafo nuevo.
* Composición del vídeo en `render.c` (`movie_draw_tick`) por defecto;
  `OTOMEDIUS_NATIVE_MOVIE=1` deja el draw del juego.
* **Audio choppy (esta sesión)**: el ffmpeg de **audio** ya **no** usa `-re`;
  decodifica por delante y llena la cola de SDL (~200–400 ms) que absorbe el
  jitter. El de vídeo sigue con `-re`. `platform_audio_write` suelta el mutex
  mientras espera cola y el mezclador dsound no escribe mientras
  `movie_active()`.

### 4.5 Mezclador dsound → SDL (esta sesión)

`0x41B2A0` **no mezcla PCM**: recorre 14 voces y para cada buffer vivo
consulta `GetCurrentPosition` y llama `Stop` al pasar la longitud
(`[voz+0x10]+0xC`). El port mezcla en un hilo:

* `dsbuffer_t` guarda `WAVEFORMATEX` (ch/rate/bits), `freq` de reproducción,
  `play_flags`, `is_primary`, `position`, fase fraccionaria.
* Registro de 4096 objetos con mutex; clones comparten `data`/formato.
* Hilo → `platform_audio_open(48000,2)`: volumen en centibelios
  (`10^(v/2000)`), pan, 8/16 bits, mono/estéreo, resampleo por fase,
  `DSBPLAY_LOOPING`, avance de `position` (imprescindible para que el hilo
  del juego pare las voces).
* `Lock` divide bloqueos que dan la vuelta; `SetFormat`/`CreateSoundBuffer`
  leen `WAVEFORMATEX`; `SetFrequency` cambia el rate; `GetStatus` informa
  `PLAYING|LOOPING`.
* `dsound_shutdown()` antes de `platform_shutdown()`; `platform_audio_*` con
  mutex; `OTOMEDIUS_DSDBG`, `OTOMEDIUS_AUDIO_DUMP`,
  `OTOMEDIUS_AUDIO_STREAM_DUMP`.
* Medido: buffers de 894 976 B (44,1k estéreo 16), 22 050 mono 8 y 352 800 B;
  volúmenes −36/−45 dB; ráfagas con pico ~380 y RMS ~33.

### 4.6 Otros

* `oto.ini`: `game_config_load()` recorre la tabla del juego (`0x5BE328`) y
  aplica 85 valores; se repite tras `memset(0x5DB130,0,0x148)`.
* `ChkSum.bin` bypass (`0x40ED00`), 22/191 ficheros no coinciden.
* `TEXTMETRICA` 56 B (no 64: sobreescribía EBP).
* `GetGlyphOutlineA` enruta por formato **exacto**; GGO_GRAY8 escala 0..64
  (la paleta `CTexFont+0x58` sólo define bien 0..64).
* FreeType mutex recursivo (el loader también loguea por la fuente).
* Bezel 1920×1080 con hueco `(288,0)-(1631,1079)`; fullscreen por defecto,
  `F11` a la ventana configurada; `OTOMEDIUS_WINDOW_SIZE`.
* `render_dump_fb()` + dump `<DUMP_FB>.quit` al pulsar Esc/cerrar.

---

## 5. Estado Actual y Bug/Bloqueo Activo

### 5.1 Verificado (headless y windowed, ~200 s)

* Boot completo sin crash: KonamiLogo visible → eAMUSELogo → Warning →
  vídeo (~60 s) → Title (`Title.pak`, `TitleSnd.pak`, `title_stream.xwb`) →
  attract (`replay000.pak`, `catch_demo02.pak`, `Player02.pak`, `Speed.pak`,
  `TwinMsil`, `CycLaser`, `Ripple`, `MultiOpt`, `Bomb`, `Stage01.pak`), sin
  `no recompiled code`, sin SEH, sin SIGSEGV.
* **Fondo del Title RESUELTO** (esta sesión): `main_visual.dds` es **RGB24**;
  antes se leía como A8R8G8B8 y el alfa salía del azul del píxel siguiente
  (12,4 % de píxeles con alfa <128 → puntos sueltos). El dump
  `/tmp/opencode/fix3400.png` muestra el fondo continuo.
* **Attract tutorial RESUELTO**: `catch_demo02` (2048×1024 RGB24) se ve en el
  present ~4600 (`/tmp/opencode/att4600.png`). `Player02` usa A1R5G5B5 y
  ahora se decodifica bien.
* **Audio**: el mezclador dsound→SDL funciona y la canción del intro suena
  limpia tras quitar `-re` al ffmpeg de audio (confirmado de oído).
* Esc en cualquier momento: `platform: Esc pressed`, se escribe
  `<DUMP_FB>.quit` y el proceso sale con `exit=0`.

### 5.2 Bloqueo activo: attract en blanco (`CDemoPlayMan` state 4)

En el attract, tras el tutorial, la pantalla se queda **blanca** (sólo el
fade a pantalla completa `fvf=44`, `tex=0`, color `0xFFFFFFFF` + la consola
de depuración roja/negra) desde el present ~5000 hasta el 9000. **No es un
problema de texturas ni de draws**: `draw=0` (nunca `DrawPrimitive`) y con
`OTOMEDIUS_DRAWDBG=1` **no aparece ningún `SKIP`/`DISCARD`** en toda la
corrida; los 48 `DrawPrimitiveUP` son de frames distintos (el stat agrupa
120 presents) y todos llegan al rasterizador.

Log medido (`OTOMEDIUS_BOOTDBG=1`, corrida offscreen a 9000):

```
bootdbg: [present 279] update #0     state=0 sub=0 io=0 slot258=00000000
bootdbg: [present 280] update #1     state=0 sub=1 io=0 slot258=10069480
bootdbg: [present 523] update #244   state=0 sub=2 io=0 slot258=00000000
bootdbg: [present 524] update #245   state=0 sub=3 io=0 slot258=10041160
bootdbg: [present 706] update #427   state=0 sub=4 io=0 slot258=00000000
bootdbg: [present 707] update #428   state=0 sub=5 io=0 slot258=10041160
bootdbg: [present 890] update #611   state=0 sub=6 io=0 slot258=00000000
bootdbg: [present 891] update #612   state=0 sub=7 io=0 slot258=10041160
bootdbg: [present 2307] update #2028 state=1 sub=0 io=0 slot258=00000000
bootdbg: [present 2308] update #2029 state=1 sub=1 io=0 slot258=10041248
bootdbg: [present 4083] update #3804 state=0 sub=8 io=0 slot258=00000000
bootdbg: [present 4084] update #3805 state=0 sub=9 io=0 slot258=10041248
bootdbg: [present 4298] DEMO state=0 slot25c=00000000 slot260=00000000 io=0 mask220=00000000
bootdbg: [present 4299] DEMO state=1 ...
bootdbg: [present 4300] DEMO state=2 ...
bootdbg: [present 4301] DEMO state=3 slot25c=10042ac0 ...
bootdbg: [present 4416] DEMO state=4 slot25c=00000000 slot260=00000000 ...
```

`CDemoPlayMan` (`0x429210`, update `0x429340`, draw `0x4294C0`) hace:

* state 0: encola `Data/replay000.pak` (id `[0x5E2024]`, rota 0..3).
* state 1: `0x419530()`; si devuelve 1 → state 2.
* state 2: `0x4142B0(0x5E1F28,0x10)` + `0x42CE60` (`CEyeCatchMan`, slot
  `0x5DE25C`).
* state 3: espera `[0x5DE25C]==0` → state 4.
* state 4: si `[0x5E2136]!=1`: `0x4142B0(0x5E1F28,0x44)` + `0x42E1F0`
  (`CGameMan`, slot `0x5DE260`), `state++`, timer=0.
* state 5: corre 0xCE4 = 3300 frames; `[0x5E294D]=1`; espera `0x5DE260`.

**El estado 4 se alcanza en el present 4416 y nunca se ve el 5 hasta el
9000.** El boot manager (`0x4547E0`) sigue en `state=0 sub=9` esperando a que
`CDemoPlayMan` libere `0x5DE258`; por eso la pantalla sigue en el fade.
No hay mensajes `1034–1036` ("CGameMan Not load pak PLAYER/POWERUP/STAGE"),
ni `1115`, ni SEH, ni `mask220=0xFFFFFFFF` en los logs.

**Hipótesis a comprobar (en orden):**

1. `0x42E1F0` (`CGameMan`) no retorna (SEH/longjmp) o tarda/bloquea, por lo
   que no se ejecuta el `state++` posterior.
2. `CGameMan` crea el manager pero deja `[0x5DE260]`/`[0x5DE220]` en un estado
   que suspende el manager loop y `0x429340` deja de actualizarse.
3. El update de `CDemoPlayMan` deja de llamarse porque el slot `0x5DE258`
   se limpia o `[obj+4]&[0x5DE220]` lo salta.
4. El fade blanco pertenece al `CEyeCatchMan`/demo y no avanza porque el
   temporizador o el estado del `CGameMan` no progresa (no porque falte un
   draw 3D).

### 5.3 Pendientes menores

* Quad nativo del vídeo `0x43F540`: la X sigue duplicada por el escalado x87
  que `0x41C500` toma de la pila (la composición de `render.c` es el camino
  por defecto).
* Rendimiento del rasterizador: ~26–36 fps (por píxel, float).
* Audio del juego en Flatpak: el mezclador necesita el dispositivo
  PulseAudio del sandbox (`--socket=pulseaudio` ya está).
* Si el demo del attract usa geometría 3D: no hay camino XYZ todavía
  (`draw_up_vertices` descarta `!(fvf&4)`); `render.c` ya guarda
  WORLD/VIEW/PROJ en `g_xform[3][16]` y `SetTransform` los alimenta.

---

## 6. Plan de Acción Inmediato (3 pasos)

### Paso 1 — Instrumentar `CGameMan` y el state 4

Añadir en `native/patches.c` (dentro del bloque `OTOMEDIUS_BOOTDBG`):

* `0x42E1F0`: log al entrar y al salir (valor de retorno/`cpu->eax`), con
  `slot260` y `mask220` antes/después.
* `0x403DB0`: log del id/format string cuando se llame (así se ve cualquier
  `1034–1038` o error que ponga `mask220=0xFFFFFFFF`).
* `0x429340`: además del cambio de estado, imprimir cada 120 presents el
  estado actual aunque no cambie, junto con `slot25c`, `slot260`, `io`,
  `mask220`.

Correr (offscreen, sin ventana):
```sh
cd /var/home/vm/Proyectos/SALA DE TRABAJO/.work/oto
SDL_VIDEODRIVER=offscreen OTOMEDIUS_BOOTDBG=1 \
  OTOMEDIUS_DUMP_FB=/tmp/opencode/dbg OTOMEDIUS_DUMP_AT=4400 \
  OTOMEDIUS_DUMP_EVERY=200 OTOMEDIUS_QUIT_AT=6002 \
  timeout -k 5 900 ../obj/otomedius-recomp . > /tmp/opencode/dbg.log 2>&1
```
Objetivo: saber si `0x42E1F0` retorna, si `state` pasa a 5 y si `mask220`
se envenena.

### Paso 2 — Arreglar el avance del attract

Según el resultado:

* Si `0x42E1F0` no retorna: localizar el punto exacto con Ghidra
  (`get_functions({functions:"0x42E1F0"})`), revisar llamadas a
  `0x4142B0`, `0x409810`, `0x409F80`, `0x410C20`, `0x4104A0` y los chequeos
  de paks (`0x5DE09C`, `0x5DE0A0`…`0x5DE0BC`); comparar con el estado real
  de la tabla de 16 paquetes.
* Si `mask220` se pone a `0xFFFFFFFF`: identificar el id de error en
  `0x403DB0` y añadirlo a la lista de errores ignorados **sólo** si es
  cosmético (como 2302/2306), nunca si oculta un fallo de carga.
* Si el estado avanza a 5 pero no se ve nada: revisar `CGameMan` (vtable,
  update/draw) y, si usa 3D, implementar el camino XYZ:
  `draw_up_vertices` con FVF `0x2` (XYZ) → transformar con
  `g_xform[WORLD]*g_xform[VIEW]*g_xform[PROJ]`, dividir por w, viewport,
  y rasterizar con **z-buffer** reutilizando `draw_tri` (interpolación
  perspective-correct de UV).

### Paso 3 — Cerrar lo que queda de audio/vídeo/rendimiento

1. Probar el mezclador dsound en el Flatpak real (`--socket=pulseaudio`) y
   ajustar volúmenes si hace falta; verificar `OTOMEDIUS_AUDIO_DUMP` con RMS
   no nulo en Title/attract.
2. Arreglar el quad nativo `0x43F540` (X duplicada): revisar la pila x87 de
   `0x41C500`/`0x41C620` o dejar la composición de `render.c` como camino
   oficial y documentarlo.
3. Rendimiento del rasterizador (~26–36 fps): perfilar `draw_tri`/
   `tex_sample` (evitar `pow`/divisiones por píxel, cachear filas, SIMD);
   opcional: resolver el `draw=0` de `DrawPrimitive`/`DrawIndexedPrimitive`
   si el juego los usa en el gameplay real.

---

### Reglas de la sesión

* Leer `docs/HANDOVER.md` entero; **§10 manda** sobre §1–§9.
* No hacer commits ni push sin pedirlo; **nunca** escribir tokens en
  ficheros (usar `GIT_ASKPASS` + variable de entorno).
* Herramientas: `sh .work/tools/build.sh <archivo.c>` (recompila nativos +
  link), `python3 .work/tools/xd.py <hex> [n]` (desensamblado),
  `python3 .work/tools/ppm2png.py in.ppm out.png [escala]`.
* Generar: `PYTHONPATH=../.work/pylib python3 tools/recomp/recomp.py --exe
  ../.work/oto/otomedius.exe --out ../.work/gen --chunk 60`.
* Correr: `cd ../.work/oto && ../obj/otomedius-recomp .`.
* Ghidra MCP activo: `tools["ghidra-mcp"].get_functions({functions:"0x…",
  fields:"entry_point,decompiled_code"})` (proyecto `gghidra`, 4 533
  funciones).
* Pruebas visuales en **ventana 16:9 pequeña**
  (`OTOMEDIUS_WINDOWED=1 OTOMEDIUS_WINDOW_SIZE=800x450`), no fullscreen;
  corridas largas con `SDL_VIDEODRIVER=offscreen` y `OTOMEDIUS_QUIT_AT`.
