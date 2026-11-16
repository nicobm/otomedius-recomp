# Otomedius (Arcade) — Handover para sesión nueva

> Copiar y pegar este documento entero como prompt inicial. El documento
> canónico y exhaustivo es `docs/HANDOVER.md`; **§11 manda sobre §1–§10**.
> Repo: `/var/home/vm/Proyectos/SALA DE TRABAJO/otomedius-recomp`.
> Estado del árbol: HEAD `c43c61f` + los cambios de la sesión del §11 **sin
> commitear** (comprobar con `git status` / `git log -1`). No hagas commits ni
> push sin pedirlo. **Nunca** escribas tokens en ficheros: el push va con
> `GIT_ASKPASS` + variable de entorno. Este repo **no contiene datos del
> juego**; el dump se traduce en la máquina del usuario.

---

## 1. Entorno y toolchain

| Item | Valor |
|---|---|
| Host / target | Linux **x86-64 only** (sin Windows/macOS/32-bit) |
| Entrega | Flatpak, runtime `org.freedesktop.Platform//26.08` + SDK |
| Pantalla | Wayland nativo + XWayland; renderer **OpenGL**; bezel + fullscreen |
| UI | **SDL3 3.4.20** (sha256 `f3bb06f5…`) |
| Compilador agente | `zig 0.17.1` como `cc` (`-O2 -std=gnu11 -w -Inative`) |
| Compilador Flatpak | SDK `cc` (GCC) `-O2 -std=c11 -w -Inative` + `pkg-config sdl3 freetype2` |
| Recompilador | Python 3 + **capstone 5.0.9** (`tools/recomp`) |
| FreeType | 2.13.2 (headers/libs en `.work/freetype_include`, `.work/freetype_lib`) |

Directorios: `native/` (capa Linux), `tools/recomp/`, `recomp/` (seeds+sha),
`tests/`, `flatpak/`, `docs/`. `.work` está en el directorio **padre**:
`/var/home/vm/Proyectos/SALA DE TRABAJO/.work`.

```sh
# compilar la capa nativa (solo los ficheros tocados) y enlazar
sh ../.work/tools/build.sh patches.c d3dx9.c d3d9.c d3d9_objects.c \
   win32_kernel32.c win32_misc.c font.c render.c x86rt.c

# regenerar el juego (SOLO si cambia native/x86rt.h; tarda y recompila 85 rc_*.c)
PYTHONPATH=../.work/pylib python3 tools/recomp/recomp.py \
    --exe ../.work/oto/otomedius.exe --out ../.work/gen --chunk 60

# test de humo reproducible (offscreen + audio dummy, 4700 presentes)
OTOMEDIUS_SMOKE_LOG=/tmp/opencode/smoke.log sh tests/smoke.sh

# corrida manual
cd ../.work/oto
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
  OTOMEDIUS_BOOTDBG=1 OTOMEDIUS_DUMP_FB=/tmp/opencode/h \
  OTOMEDIUS_DUMP_AT=4400 OTOMEDIUS_DUMP_EVERY=600 \
  OTOMEDIUS_QUIT_AT=9000 ../obj/otomedius-recomp . \
  > /tmp/opencode/h.log 2>&1
```

**Reglas de pruebas**
- `SDL_AUDIODRIVER=dummy` **siempre** en tests: si hay dispositivo, el mixer
  dsound→SDL suena de verdad.
- Logs grandes = tokens: `OTOMEDIUS_QUIT_AT` bajo, `grep`/`head`/`tail`, nada de
  volcar logs completos al contexto.
- Para dumps visuales: `ppm2png.py` está en `.work/tools/`.
- `native/x86rt.h` es el runtime: tocarlo obliga a regenerar y recompilar los 85
  `rc_*.c`. Los cambios de la sesión anterior son solo `native/*.c`.

---

## 2. Estado actual (verificado)

El juego arranca de punta a punta:

`KonamiLogo` → `eAMUSELogo` → `Warning` → vídeo (~60 s) → `Title` →
eye-catch → **attract con `CGameMan` y state 5** → HUD 2D visible.

Cadena medida con `OTOMEDIUS_BOOTDBG=1`:

```
present 4160  DEMO state=0
present 4163  DEMO state=3 slot25c=10176d78
present 4277  DEMO state=4
present 4277  CGameMan ctor this=1018bca0
present 4277  CGameMan ctor returned eax=1018bca0 mask220=00000000
present 4278  DEMO state=5 slot260=1018bca0
present 4398  DEMO state=5 (se mantiene)
```

- Corrida headless con audio dummy hasta `OTOMEDIUS_QUIT_AT=9000`: **sin crash**,
  sin `no recompiled code`, sin agotar thunks.
- Frame 6000: 129 quads / 258 vértices; HUD visible (retrato Player 1, barras,
  botones). Fondo/escena **negro** (3D pendiente).
- `tests/smoke.sh` pasa: state 5, `mask220=0`, exit 0.

---

## 3. Qué se arregló en la sesión anterior (y no hay que volver a tocar)

1. **`ID3DXEffect` stub real** (`native/d3dx9.c`) — la causa raíz del bloqueo.
   `D3DXCreateEffect` devolvía `E_FAIL`, el build de `Stage01.pak` abortaba
   (2302/2306) y el pak nunca quedaba ready; `CGameMan::CGameMan` (0x42E1F0)
   logueaba 1036 "Not load pak STAGE" y `0x403DB0` ponía
   `[0x5DE220] = 0xFFFFFFFF`. Ahora devuelve `S_OK` con un objeto de 79
   métodos (`GetParameter*`/`GetTechnique*`/`GetPass*` no nulos, `Begin`
   escribe `passes=1`, `SetTexture`→stage 0, `SetMatrix` guarda la matriz).
2. **`AddFontMemResourceEx` real** (`win32_misc.c` + `font.c`): registra las
   fuentes del juego (`DFGothic-SU`, `DFMincho-SU`, `DFMaruGothic-Bd`,
   `DFPOP2-W9`, `DFPOPCorn-W12`, `DFGothicP-W5`) con `FT_New_Memory_Face`.
   `OTOMEDIUS_SYSFONT=1` vuelve a la fuente del sistema.
3. **Arenas de memoria** (`win32_kernel32.c`): `HEAP 0x10000000–0x20000000`,
   `MAP 0x20000000–0x30000000`, `PAGE 0x30000000–0x60000000`. La ventana de
   `MapViewOfFile` se solapaba con el heap y las paks del Title escribían encima
   de listas de jobs (`fn = 0xc954c1a1`). `UnmapViewOfFile` ahora libera.
4. **Vtables COM compartidas** (`d3d9.c`): una por clase, no una por objeto.
   Antes ~3000 texturas agotaban la tabla fija de 0x10000 thunks.
5. **`D3DXVec3TransformArray` / `NormalArray`**: firma real de **6** args
   `(pOut, OutStride, pV, VStride, pM, n)`. La tabla decía 5: dejaba 4 bytes por
   llamada en la pila y corrompía direcciones de retorno. `Vec3Transform` ahora
   escribe `w`.
6. **`GetBackBuffer` + `s_GetDesc`**: surface real 1280×1024 A8R8G8B8 y
   `D3DSURFACE_DESC` relleno; `0x4505E0` los usa para crear su render target.
7. **Workaround 3D** (`patches.c`, `0x4ECBF3`): si `CPlayer02::Init` encuentra
   `scene+0x150` vacío (interfaz de animación `.x` que no cargamos), instala un
   objeto COM vacío (`model_stub`). Evita el `call [0]`; **no dibuja la nave**.
8. **`tests/smoke.sh`** nuevo y utilidades: `OTOMEDIUS_TRIPWIRE` (`x86rt.c`),
   `OTOMEDIUS_TEXDUMP` (`render.c`), `OTOMEDIUS_EFFECTDBG`, `OTOMEDIUS_TEXDBG`,
   `OTOMEDIUS_MALLOCDBG`, `OTOMEDIUS_WATCHDOG`.

---

## 4. Bloqueo activo: la capa 3D

El attract ya corre, pero la escena 3D no se dibuja (`draw=0` en todos los
frames; el HUD sí sale porque es 2D).

### 4.1 Modelos `.x`
- `CPlayer02`: vtable **`0x56CAFC`**, RTTI **`0x58AE20`**; el método índice 2 es
  **`0x4ECBE0`**. Ese método hace:
  ```
  0x4ECBF3  mov eax,[esi+0x94]      ; scene
  0x4ECBF9  mov eax,[eax+8]         ; model root (b)
  0x4ECBFC  mov ecx,[eax+0x150]     ; animation interface  ← 0 en este port
  0x4ECC02  mov ecx,[ecx+0x44]      ; null → xor ecx,ecx → call [0]
  ```
- La interfaz la produce el loader `.x`: **`0x414960`** llama a
  `D3DXLoadMeshHierarchyFromXInMemory` en **`0x4149FE`**. Nuestro stub devuelve
  `S_OK` con frame/animación NULL.
- La constructora base de Player es **`0x4EA0E0`**; `CPlayer02` está en
  **`0x4ECA11`–`0x4ECA50`** (pone `[esi]=0x56CAFC` y `or [b+0x154],1`).
- Decidir entre: (a) parser `.x` mínimo (frames + mesh + materiales) usando la
  `ID3DXAllocateHierarchy` que pasa el juego, o (b) HLE de los datos que el
  juego espera, leyendo los chunks reales de `Player02.pak`/`Stage01.pak`.
  Los `.x` están dentro de los paks; `Stage01.pak` son ~10,7 MB.

### 4.2 Pipeline XYZ / draw
- `render.c` ya guarda `WORLD/VIEW/PROJ` en `g_xform[3][16]` y la última matriz
  del efecto en `g_effect_xform` (`render_set_effect_matrix`).
- Faltan: transformar vértices XYZ (FVF `0x142`/`0x144`), dividir por `w`,
  viewport, z-test, UV perspective-correct, y enlazar `DrawPrimitive` /
  `DrawIndexedPrimitive` con los vertex/index buffers (ya hay `SetStreamSource`,
  `SetIndices`, `Lock`).
- `dev_DrawPrimitive` ya llama a `render_draw(..., indexed=0)`; el problema es
  que el camino de vértices no interpreta posiciones 3D y descarta.

### 4.3 HUD / texto
- La tira **512×64 DXT5** (`obj 60915380`) decodifica bien (es una fila de
  botones hexagonales). En el frame 4400 se dibuja un quad ancho
  `(368,40)–(636,40)` con `u 0.000–0.524`, y quads de 16 px con
  `u 0.531–0.563`: revisar UV por celdas, blending y estados.
- El texto amarillo ilegible del HUD probablemente comparte esa causa
  (UV/escala/estado), no el decode DXT.
- `FreeLocal error 1172/1173/1174` al teardown **solo con `BOOTDBG`**; en
  corrida normal no aparecen. Sospecha: datos `.x` ausentes + objeto falso.

### 4.4 Rendimiento / detalles
- Rasterizador software ~26–36 fps. Quad nativo `0x43F540` con X duplicada.
- El mezclador dsound→SDL y el vídeo están resueltos; el audio en Flatpak
  necesita `--socket=pulseaudio`.

---

## 5. Plan inmediato (3 pasos)

1. **Entender qué espera `CPlayer02` de `+0x150`**
   - Desensamblar `0x414960`, `0x4EA0E0`, `0x4ECA11`, `0x4ECBE0` y los métodos
     de la vtable `0x56CAFC`.
   - Ver la `ID3DXAllocateHierarchy` del juego (probablemente en `0x414960`) y
     qué hace con `ppFrame`/`ppAnim`.
   - Decidir parser `.x` vs HLE. Si es HLE, extraer los chunks reales del pak y
     rellenar las estructuras que el juego espera.
2. **Pipeline XYZ en `render.c`**
   - Implementar FVF `0x142`/`0x144`: `v' = v · W · V · P`, `/w`, viewport,
     z-buffer, UV perspective-correct.
   - Enlazar `DrawPrimitive`/`DrawIndexedPrimitive` con los buffers.
   - Verificar con `OTOMEDIUS_DRAWDBG=<frame>` y dumps PPM/PNG.
3. **HUD y rendimiento**
   - Corregir el quad ancho de la tira de botones (UV por celdas/estado) y el
     texto.
   - Perfilar `draw_tri`/`tex_sample` (evitar pow/divisiones por píxel).

---

## 6. Direcciones y globals útiles

| Cosa | Valor |
|---|---|
| `CDemoPlayMan::Update` | `0x429340` (state en `this+0xA`) |
| `CGameMan::CGameMan` | `0x42E1F0`; slot `0x5DE260`; mask `0x5DE220` |
| `CDemoPlayMan` slot | `0x5DE258`; eye-catch `0x5DE25C` |
| `0x403DB0` | log + **pone `[0x5DE220] = 0xFFFFFFFF`** (suspende managers) |
| 2302/2306 | `0x418C6A` / `0x41812B` (build de pak) |
| 1036 STAGE | `0x42E1F0` (`0x55BD5C`) |
| `AddFontMemResourceEx` | `0x4190FE` (2313 si falla) |
| `CPlayer02` vtable | `0x56CAFC`, método 2 `0x4ECBE0` |
| Loader `.x` | `0x414960`, llamada `0x4149FE` |
| Back buffer → texture | `0x4505E0` |
| Device D3D9 | `[0x5E1F20]`; `g_xform` en `render.c` |
| Job list runner | `0x4095A0` (registros `{prio, fn, arg}` de 12 B) |

---

## 7. Herramientas y diagnóstico

- **Ghidra MCP** del handover: no está en el catálogo de esta sesión.
- **REA** (MCP) está disponible pero su provider Ghidra **falla**:
  `GhidraLaunchError` (`launch.properties` del Flatpak). Se usó
  `capstone` local, `../.work/gen/rc_*.c` e instrumentación runtime.
- `OTOMEDIUS_WATCHDOG=1`: estado de todos los hilos cada 2 s.
- `OTOMEDIUS_TRIPWIRE=<addr[,addr…]>`: detecta escrituras salvajes.
- `OTOMEDIUS_TEXDUMP=WxH`: PPM de una textura decodificada.
- `OTOMEDIUS_DRAWDBG=<frame>`: draws, FVF, textura, estados, matrices.
- `OTOMEDIUS_TEXFMT=1` / `OTOMEDIUS_TEXDBG=1`: formatos DDS / creaciones.
- `OTOMEDIUS_EFFECTDBG=1`: blobs de `D3DXCreateEffect`.
- `OTOMEDIUS_MALLOCDBG=1`: `MallocLocal` (junto a `OTOMEDIUS_BOOTDBG`).

---

## 8. Ficheros clave (sesión anterior)

```
M native/d3dx9.c           ID3DXEffect; Vec3TransformArray 6 args; w
M native/d3d9.c            vtables compartidas; back buffer real
M native/d3d9_objects.c    GetDesc de surface; TEXDBG
M native/win32_kernel32.c  arenas heap/map; UnmapViewOfFile real
M native/win32_misc.c      AddFontMemResourceEx real
M native/font.c            fuentes en memoria; OTOMEDIUS_SYSFONT
M native/patches.c         0x4ECBF3 workaround; BOOTDBG/MALLOCDBG acotado
M native/render.c          render_set_effect_matrix; OTOMEDIUS_TEXDUMP
M native/x86rt.c           OTOMEDIUS_TRIPWIRE
A tests/smoke.sh           test de humo headless
M docs/HANDOVER.md         §11
M docs/HANDOVER_PROMPT.md  este documento
```

---

## 9. Reglas de la sesión

- No commits ni push sin pedirlo.
- Nunca tokens en ficheros; `GIT_ASKPASS` + entorno.
- Tests con `SDL_AUDIODRIVER=dummy` (silencio).
- Logs mínimos; no volcar logs enteros.
- Un cambio en `native/x86rt.h` ⇒ regenerar y recompilar los 85 `rc_*.c`.
- Antes de tocar algo dudoso, `git diff` para no pisar trabajo del usuario.
