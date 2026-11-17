# Otomedius (Arcade) — Handover para sesión nueva

> Copiar y pegar este documento entero como prompt inicial. El documento
> canónico y exhaustivo es `docs/BOOKLET.md`; **§12 manda sobre §1–§11**.
> Las reglas de operación (`INICIAR` / `TERMINAR`) están en `AGENTS.md` y en
> `docs/BOOKLET.md` §0.
> Repo: `/var/home/vm/Proyectos/SALA DE TRABAJO/otomedius-recomp`.
> Estado del árbol: **limpio** en el commit de esta sesión (ver `git log -1`;
> el hash se informa al cerrar la sesión). No hagas commits ni push sin
> pedirlo. **Nunca** escribas tokens en ficheros: el push va con `GIT_ASKPASS`
> + variable de entorno. Este repo **no contiene datos del juego**; el dump se
> traduce en la máquina del usuario.

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
sh ../.work/tools/build.sh render.c d3dx9.c patches.c font.c win32_misc.c

# regenerar el juego (SOLO si cambia native/x86rt.h; tarda y recompila 85 rc_*.c)
PYTHONPATH=../.work/pylib python3 tools/recomp/recomp.py \
    --exe ../.work/oto/otomedius.exe --out ../.work/gen --chunk 60

# test de humo reproducible (offscreen + audio dummy, 4700 presentes)
OTOMEDIUS_SMOKE_LOG=/tmp/opencode/smoke.log sh tests/smoke.sh

# corrida manual
cd ../.work/oto
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
  OTOMEDIUS_BOOTDBG=1 OTOMEDIUS_QUIT_AT=4700 ../obj/otomedius-recomp .
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
present 4251  CGameMan ctor returned, eax=… mask220=00000000
present 4252  DEMO state=5 slot260=… mask220=00000000
present 4286  DEMO state=5 (se mantiene)
```

- Corrida headless con audio dummy hasta `OTOMEDIUS_QUIT_AT=9000`: sin crash,
  sin `no recompiled code`, sin agotar thunks.
- En esta sesión se verificó `state=5`/`mask220=0` en corridas hasta 4400/4530;
  el smoke formal de 4700 **no se rerunneó** (ver §12.5 del booklet).
- **Pipeline XYZ nuevo** en `render.c`: `W*V*P`, `/w`, viewport, z-buffer,
  UV perspective-correct y `DrawPrimitive`/`DrawIndexedPrimitive` con
  vertex/index buffers. El test `OTOMEDIUS_TEST_3D=1` dibuja un cubo sólido.
- **Nave HLE visible**: `D3DXLoadMeshHierarchyFromXInMemory` construye una
  jerarquía de 5 frames con el `ID3DXAllocateHierarchy` del juego y cuelga un
  contenedor con una nave sintética. `OTOMEDIUS_SHIP_OVERLAY=1` la muestra en
  el centro; el HUD 2D se dibuja completo.
- **Texto del HUD**: la mayor parte ya se lee. La línea larga del warning rojo
  sigue solapándose.

Lo que falta: el **3D real del juego** (`.x` + cámara) y el texto largo.

---

## 3. Qué se arregló en la sesión anterior (y no hay que volver a tocar)

1. **Test de interior del triángulo invertido** (`native/render.c`). El
   rasterizador saltaba el interior y dibujaba el complemento. Con eso las
   caras 3D desaparecían y los quads 2D muestreaban las UV del otro triángulo.
   Ahora se dibuja solo si `w0,w1,w2` tienen el mismo signo.
2. **`GetCharABCWidthsA` con `ABC` de 12 bytes** (`native/font.c`,
   `native/win32_misc.c`). La API usa `int abcA; UINT abcB; int abcC` (tres
   dwords); el layout compacto de 6 bytes rompía los avances y solapaba texto.
3. **`D3DXLoadMeshHierarchyFromXInMemory`: `ppFrame=arg6`, `ppAnim=arg7`**
   (`native/d3dx9.c`). El stub los tenía invertidos; con la jerarquía real el
   juego llamaba a la vtable del frame y moría en `10041408`.
4. **Heap local**: crear 70–81 frames por cada `.x` (~80 cargas) agotaba el
   heap y abortaba el pak (`2207/2206/2310/2306`). Ahora solo se sintetiza
   jerarquía para el `.x` que contiene `p02_*` (modelo del jugador), con 5
   frames, y `D3DXFrameDestroy` libera los frames con el `DestroyFrame` del
   juego.
5. **Protocolo de sesión**: `AGENTS.md` (nuevo) y `docs/BOOKLET.md` §0 con
   `INICIAR` / `TERMINAR`. `docs/HANDOVER.md` se renombró a `docs/BOOKLET.md`.

---

## 4. Bloqueos activos

### 4.1 Modelo `.x` real
- `Data/Player02.pak` es un **CAB (`MSCF`)** con `Player02.bin`; el `.bin` es
  un contenedor de chunks: `TXGP` (texturas DDS), `POF0`, `MESH`, `CONF`.
- El chunk `MESH`: `u32 count=1`, `u32 name_size=0x0c`,
  `u32 data_size=0x3df2e0`, `"player02.x\0\0"`, y un `.x` **binario**
  (`xof 0303bin 0032`) de 4.059.840 bytes. Templates: `Frame`,
  `FrameTransformMatrix`, `Mesh`, `MeshMaterialList`, `MeshNormals`,
  `MeshTextureCoords`, `SkinWeights`, `AnimationKey`. El primer frame de datos
  (`p02_master`) está hacia `0x582d2`.
- `CPlayer02`: vtable `0x56CAFC`, `Init` `0x4ECBE0`. `CModel` (0x48 bytes):
  `+0` root frame, `+8` array de nodos (entradas de 0x70), `+0x14` anim.
  `CPlayer02::Init` toca `node[3]`; otra clase toca `node[68]`.
- Contenedor del juego: vtable `0x5590AC` (estático) / `0x559088` (skinned);
  `CreateMeshContainer` devuelve `container+4`; el draw original `0x415DF0`
  usa el renderer `[container+0x28]` y `mesh->DrawSubset`. El HLE sustituye
  esa vtable por una nativa.

### 4.2 Cámara / VIEW
- El juego usa `ID3DXEffect::SetMatrix` para el camino de shaders, pero el stub
  devuelve un handle constante (`0xe0000001`) y solo guarda la última matriz.
  No se distingue World/View/Projection. `render_draw_ship` centra la nave
  cuando no hay VIEW; es un workaround, no la cámara final.
- Las matrices de proyección sí se ven (`[0.640 … | 0.800 … | 1.010 1 | -30.303]`),
  y el mundo llega como primer argumento del draw del contenedor.

### 4.3 Texto largo del warning
- La línea larga del warning rojo sigue solapándose. El `ABC` de 12 bytes
  mejoró el resto; falta revisar el wrapping/avance del renderer de texto para
  cadenas largas y el blending.

### 4.4 Rendimiento
- Rasterizador software ~26–36 fps. Perfilar `draw_tri`/`tex_sample` cuando el
  3D real esté en marcha.

---

## 5. Plan inmediato (3 pasos)

1. **Parser `.x` mínimo / `ID3DXMesh` real**
   - Leer el `.x` binario del chunk `MESH`: frames, vértices, caras,
     materiales, UVs y normales.
   - Sustituir la nave sintética por la malla real; mantener el HLE como
     fallback (`OTOMEDIUS_NO_SHIP=1`).
2. **Cámara real**
   - Guardar matrices de efecto por handle (o identificarlas por forma) y
     distinguir World/View/Projection.
   - Quitar el centrado de `render_draw_ship` cuando haya VIEW.
3. **HUD/texto y rendimiento**
   - Corregir la línea larga del warning; revisar blending/estado.
   - Perfilar `draw_tri`/`tex_sample`.

---

## 6. Direcciones y globals útiles

| Cosa | Valor |
|---|---|
| `CDemoPlayMan::Update` | `0x429340` (state en `this+0xA`) |
| `CGameMan::CGameMan` | `0x42E1F0`; slot `0x5DE260`; mask `0x5DE220` |
| `CDemoPlayMan` slot | `0x5DE258`; eye-catch `0x5DE25C` |
| `0x403DB0` | log + **pone `[0x5DE220] = 0xFFFFFFFF`** (suspende managers) |
| `CPlayer02` vtable / Init | `0x56CAFC` / `0x4ECBE0` |
| `CModel` | 0x48 bytes; `+0` root frame, `+8` nodos (0x70), `+0x14` anim |
| `D3DXFRAME` layout | Name +0, matrix +4, pMeshContainer +0x44, sibling +0x48, child +0x4c |
| Loader `.x` | `0x414960`; allocator `0x559074`; `CreateFrame` `0x4167A0` |
| Contenedor | vtable `0x5590AC` / `0x559088`; draw `0x415DF0` |
| `ID3DXEffect::SetMatrix` | handle constante `0xe0000001`; hace falta mapear por handle |
| Back buffer → texture | `0x4505E0` |
| Device D3D9 | `[0x5E1F20]`; `g_xform` en `render.c` |
| Job list runner | `0x4095A0` (registros `{prio, fn, arg}` de 12 B) |

---

## 7. Herramientas y diagnóstico

- **Ghidra MCP** del handover: no está en el catálogo de esta sesión.
- **REA** (MCP) está disponible pero su provider Ghidra **falla**:
  `GhidraLaunchError` (`launch.properties` del Flatpak). Se usó `capstone`
  local, `../.work/gen/rc_*.c` e instrumentación runtime.
- `OTOMEDIUS_WATCHDOG=1`: estado de todos los hilos cada 2 s.
- `OTOMEDIUS_TRIPWIRE=<addr[,addr…]>`: detecta escrituras salvajes.
- `OTOMEDIUS_TEXDUMP=WxH`: PPM de una textura decodificada.
- `OTOMEDIUS_DRAWDBG=<frame>`: draws, FVF, textura, estados, matrices.
- `OTOMEDIUS_3DDBG=1`: matrices del efecto, draws de la nave y conteo de nodos.
- `OTOMEDIUS_TEST_3D=1`: cubo XYZ de prueba (transform + z-buffer).
- `OTOMEDIUS_SHIP_OVERLAY=1`: dibuja la nave HLE encima de todo (diagnóstico).
- `OTOMEDIUS_SHIP_CENTER=0`: desactiva el centrado de la nave.
- `OTOMEDIUS_USE_EFFECT_XFORM=1`: usa `g_effect_xform` como WVP.
- `OTOMEDIUS_NO_SHIP=1`: desactiva la jerarquía HLE (vuelve al stub NULL).
- `OTOMEDIUS_TEXFMT=1` / `OTOMEDIUS_TEXDBG=1`: formatos DDS / creaciones.
- `OTOMEDIUS_EFFECTDBG=1`: blobs de `D3DXCreateEffect`.
- `OTOMEDIUS_MALLOCDBG=1`: `MallocLocal` (junto a `OTOMEDIUS_BOOTDBG`).

---

## 8. Ficheros clave (sesión anterior)

```
M native/render.c          pipeline XYZ, z-buffer, viewport, UV perspective-
                           correct, draw_vb, HLE ship, test 3D, fix inside test
M native/d3dx9.c           jerarquía HLE, D3DXFrameDestroy, fix ppFrame/ppAnim,
                           3DDBG de matrices de efecto
M native/d3d9.h            render_draw_ship, buffer_data
M native/d3d9_objects.c    buffer_data() para el rasterizador
M native/font.c            ABC de 12 bytes
M native/win32_misc.c      fallback de ABC de 12 bytes
M native/patches.c         BOOTDBG de 0x415CE0 (conteo de nodos)
M README.md                estado y protocolo de sesión
M docs/BOOKLET.md          §12
M docs/HANDOVER_PROMPT.md  este documento
A AGENTS.md                protocolo INICIAR / TERMINAR
R docs/HANDOVER.md         renombrado a docs/BOOKLET.md
```

---

## 9. Reglas de la sesión

- Protocolo completo: `AGENTS.md` y `docs/BOOKLET.md` §0.
- No commits ni push sin pedirlo (`TERMINAR`).
- Nunca tokens en ficheros; `GIT_ASKPASS` + entorno.
- Tests con `SDL_AUDIODRIVER=dummy` (silencio).
- Logs mínimos; no volcar logs enteros.
- Un cambio en `native/x86rt.h` ⇒ regenerar y recompilar los 85 `rc_*.c`.
- Antes de tocar algo dudoso, `git diff` para no pisar trabajo del usuario.
