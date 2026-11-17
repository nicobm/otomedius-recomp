# Otomedius (Arcade) — Handover para sesión nueva

> Pegar este documento entero como prompt inicial. El documento canónico y
> exhaustivo es `docs/BOOKLET.md`; **§14 manda sobre §1–§13**.
> Las reglas de operación (`INICIAR` / `TERMINAR`) están en `AGENTS.md` y en
> `docs/BOOKLET.md` §0.
> Repo: `/var/home/vm/Proyectos/SALA DE TRABAJO/otomedius-recomp`.
> `INICIAR` alcanza con el **token** de GitHub: la cuenta, el repo, el email y
> la rama salen de `git remote`/`git config` (no hace falta escribirlos).
> Estado del árbol: **limpio** en el commit de esta sesión. No hagas commits ni
> push sin `TERMINAR`. **Nunca** escribas tokens en ficheros: el push va con
> `GIT_ASKPASS` + variable de entorno de un único comando. Este repo **no
> contiene datos del juego**; el dump se traduce en la máquina del usuario.

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
sh ../.work/tools/build.sh render.c d3dx9.c d3d9.c d3d9_objects.c

# regenerar el juego (SOLO si cambia native/x86rt.h; tarda y recompila 85 rc_*.c)
PYTHONPATH=../.work/pylib python3 tools/recomp/recomp.py \
    --exe ../.work/oto/otomedius.exe --out ../.work/gen --chunk 60

# test de humo reproducible (offscreen + audio dummy, 4700 presentes)
OTOMEDIUS_SMOKE_LOG=/tmp/opencode/smoke.log sh tests/smoke.sh

# corrida manual
cd ../.work/oto
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
  OTOMEDIUS_BOOTDBG=1 OTOMEDIUS_QUIT_AT=4380 \
  OTOMEDIUS_DUMP_FB=/tmp/opencode/f.ppm OTOMEDIUS_DUMP_AT=4360 \
  ../obj/otomedius-recomp .
```

**Reglas de pruebas**
- `SDL_AUDIODRIVER=dummy` **siempre** en tests: si hay dispositivo, el mixer
  dsound→SDL suena de verdad.
- Logs grandes = tokens: `OTOMEDIUS_QUIT_AT` bajo y `grep`/`head`/`tail`.
- Dumps visuales: `OTOMEDIUS_DUMP_FB=/tmp/opencode/f.ppm` +
  `OTOMEDIUS_DUMP_AT=n` (o `DUMP_EVERY=n`); convertir con
  `../.work/tools/ppm2png.py`.
- `native/x86rt.h` es el runtime: tocarlo obliga a regenerar y recompilar los 85
  `rc_*.c`.
- Al probar una API nueva, comprobar que su `.o` es más nuevo que el `.c`.

---

## 2. Estado actual (verificado en esta sesión)

El juego arranca de punta a punta y **el smoke de 4700 pasa**:

`KonamiLogo` → `eAMUSELogo` → `Warning` → vídeo (~60 s) → `Title` →
eye-catch → **attract con `CGameMan` y state 5** → HUD 2D → **modelo 3D del
jugador visible y texturado**.

- `tests/smoke.sh`: **PASS** (`DEMO state=5`, `mask220=00000000`, sin
  `no recompiled code`, sin agotar thunks).
- **El modelo se ve**: `render: frame 4320 … model=6/7998 clip=0 zout=0
  px_model>0` (presentes ≈4230–4430; después la cámara entra dentro de
  `zn=30` y el modelo se recorta, que es lo correcto).
- **Cámara real**: `GetTransform(PROJECTION)` (0x414e96) y la matriz
  cámara→mundo del nodo (`nodo+0x30`, invertida). El VIEW del dispositivo es
  cero a propósito.
- **Luz real**: el efecto `LightDir`/`LightCol` (handles distintos).
- **Texturado**: atlases `p02_body01`, `p02_bike01`, `p02_gun01`, `p02_hair`
  decodificados desde la imagen del pak en memoria emulada y enlazados por
  nombre de malla.
- **DXT1/3/5 corregido**: la mezcla 1/3-2/3 se hace por canal (antes metía
  motas de color en *todas* las texturas DXT).

Lo que falta: el mapeo material→textura real del motor, el escenario/enemigos,
el cartel de red, `REPLAY RAND ERROR`, el texto largo y los logos del boot.

---

## 3. Qué se arregló en esta sesión (y no hay que volver a tocar)

1. **Visibilidad (causa raíz)**: tabla `D3DCMP_*` corrida en `draw_tri`
   (`native/render.c`). El default `LESSEQUAL` actuaba como `GREATEREQUAL` y
   descartaba todas las caras 3D (y el quad de composición). Ver §14.1.
2. **`GetTransform`** devuelve WORLD/VIEW/PROJ (identidad por defecto) —
   `dev_GetTransform` + `render_get_transform`.
3. **Composición del modelo**: `frameWorld × inverse_rígida(cámara)`
   (`hle_mat_invert_rigid`); se eliminaron `default_view` y el centrado.
4. **z-buffer por target**: `zbuf_bind()` con el stride del target actual.
5. **Handles de efecto por parámetro** (`eff_param_handle`,
   `eff_get_param_by_name`): `View`, `Proj`, `LightDir`, `LightCol`,
   `SeaParam`, `FogParam`, `FogColor`.
6. **Texturas**: nombres desde el pak en memoria (`<name>.dds` antes del
   header) + barrido único de la memoria + decode bajo demanda. El `.x` no
   trae `TextureFilename`; el mapeo es por nombre de malla.
7. **DXT**: interpolación por canal en `dxt_color_block`
   (`native/d3d9_objects.c`).

---

## 4. Bloqueo / siguiente objetivo

**Objetivo recomendado: el mapeo material→textura real del motor.**
El draw del contenedor del juego (`0x416220`) construye una tabla de texturas
en `contenedor+0x60` desde un manager en `contenedor+0x20` (`0x416580`), y su
init es `0x4163e0` (llamado desde `CreateMeshContainer` 0x416810). Nuestra HLE
sustituye el contenedor, así que ese camino nunca corre. Dos vías:

1. **Replicar el contenedor**: crear un contenedor con el layout del juego
   (`+0x20` manager, `+0x28` device, `+0x60` tabla de texturas, `+0x40` refcount)
   y llamar a `0x4163e0` con el `pAllocateHierarchyData` real, o
  2. **Llamar al lookup** `0x416580` / `0x51ba2e` (`D3DXFrameFind`) desde la HLE
     con el nombre del modelo y los materiales.

Mientras tanto el mapeo por nombre (`p02_*`) cubre las 6 mallas del jugador.

**Segundo objetivo: escenario/enemigos.** `native/xmodel.c` tiene un store
global (8 mallas, 8192 verts) y `D3DXLoadMeshHierarchyFromXInMemory` retorna 0
para todo `.x` que no sea del jugador (crear frames para los ~80 rompía el heap
del juego: `2207:Failed Alloc FrameInfs`). Para dibujar el escenario hace falta
un store por modelo y una tabla de contenedores dinámica
(`HLE_MAX_CONTAINERS` = 16, `XM_MAX_MESHES` = 8).

---

## 5. Plan inmediato (4 pasos)

1. **Cartel de red**: identificar qué función de estado consulta el juego para
   dibujar «ネットワーク接続状況確認中» y hacer que el HLE de `libbms`
   (`native/avs.c`) responda "conectado", o silenciar el cartel.
2. **`REPLAY RAND ERROR`**: averiguar qué RNG compara el replay (el mensaje está
   en `0x55beb0`) y si el CRT recompilado usa la secuencia de MSVC.
3. **Material→textura** (ver §4) y blending/alpha del material
   (`p02_transparenceShape`).
4. **Texto largo del warning** y logos del boot (§4.3, §13.8).

---

## 6. Reportes del usuario pendientes de diagnóstico

1. **Logos de Konami/eAMUSE y warning "warpeados"** mientras animan. Descartado:
   aspecto (escala uniforme en el compositor, 5:4 nativo). Falta comparar un
   dump del boot (`OTOMEDIUS_DUMP_FB` + `DUMP_AT` 200/400/800) contra la
   referencia.
2. **Audio del vídeo "saltaba"**: `platform_audio_submit` descarta el chunk si
   la cola supera ~200 ms; ahora loguea `audio: dropped N chunk(s), queue full`.
3. **Cartel rojo de red** (nuevo): ver §5.1.

---

## 7. Direcciones y globals útiles

| Cosa | Valor |
|---|---|
| `CDemoPlayMan::Update` | `0x429340` (state en `this+0xA`) |
| `CGameMan::CGameMan` | `0x42E1F0`; slot `0x5DE260`; mask `0x5DE220` |
| `CDemoPlayMan` slot | `0x5DE258`; eye-catch `0x5DE25C` |
| `CPlayer02` vtable / Init | `0x56CAFC` / `0x4ECBE0` (`stage obj` en `0x4ECBF3`) |
| `CModel` | 0x48 bytes; `+0` root frame, `+8` nodos (0x70), `+0x14` anim |
| `D3DXFRAME` layout | Name +0, matrix +4, pMeshContainer +0x44, sibling +0x48, child +0x4c |
| Loader `.x` | `0x414960`; allocator `0x559074`; `CreateFrame` `0x4167A0`; `CreateMeshContainer` `0x416810` |
| Contenedor draw / init / tex table | `0x416220` / `0x4163e0` / `0x416580` (+0x20 manager, +0x60 tabla, +0x40 refcount) |
| Node walk | `0x415810` → `[container_vt+4](escena, nodo+0x30, nodo+0x20)` |
| Proyección | `D3DXMatrixPerspectiveFovLH` → `0x5e26f0`; `SetTransform(PROJ, 0x5e31f0)`; fovy=1.7921, aspect=1.25, zn=30, zf=3000 |
| Efecto `SetMatrix` (View/Proj/LightDir/LightCol) | `0x414ead` / `0x414edb` / `0x414ef2` / `0x414f09` |
| Textura de escena (256×256) | `0x5E1EB8`; quad de composición `0x450316`/`0x5bebe0` |
| Device D3D9 | `[0x5E1F20]`; `g_xform` en `render.c` |
| Job list runner | `0x4095A0` (registros `{prio, fn, arg}` de 12 B) |
| Mensaje de red | `REPLAY RAND ERROR %d/%d` en `0x55beb0` |

---

## 8. Herramientas y diagnóstico

- **REA** (MCP) está disponible. Para leer una función concreta alcanza con el
  código recompilado: `../.work/gen/rc_*.c` (una función por `fn_XXXXXXXX`, con
  los comentarios de cada instrucción).
- `OTOMEDIUS_3DDBG=1`: draw del contenedor, matrices, `SetMatrix`,
  `GetParameterByName`, `PerspectiveFovLH`.
- `OTOMEDIUS_ORDERDBG=<present>`: orden desde ese present.
- `OTOMEDIUS_RTDBG=1`: render targets (incluye back buffer) + present.
- `OTOMEDIUS_VPDBG=1`: `SetViewport`/`SetTransform`/`PerspectiveFovLH`.
- `OTOMEDIUS_TEXDBG=1`: `CreateTexture`, nombres de textura, `SetTexture`.
- `OTOMEDIUS_HARVESTDUMP=1`: volcado de cada textura decodificada.
- `OTOMEDIUS_MODELTEX=<file.dds>`: fuerza la textura del modelo.
- `OTOMEDIUS_PAKDBG=<path>` / `OTOMEDIUS_PAKDUMP=<path>` /
  `OTOMEDIUS_PAKSCAN=<present>`: chunks y nombres del pak en memoria.
- `OTOMEDIUS_EFFECTDUMP=<dir>`: blobs de efecto.
- `OTOMEDIUS_WATCHDOG=1`, `OTOMEDIUS_TRIPWIRE=<addr>`, `OTOMEDIUS_TEXDUMP=WxH`,
  `OTOMEDIUS_TEXFMT=1`, `OTOMEDIUS_DRAWDBG=<present>`, `OTOMEDIUS_MALLOCDBG=1`.
- Ficheros volcados útiles: `/tmp/opencode/tx4.ppm` (modelo texturado),
  `/tmp/opencode/pak.x` (el `.x` extraído), `/tmp/opencode/pakimg.bin`
  (imagen del pak, ~70 MB).

---

## 9. Reglas de la sesión

- Protocolo completo: `AGENTS.md` y `docs/BOOKLET.md` §0.
- No commits ni push sin `TERMINAR`.
- Nunca tokens en ficheros; `GIT_ASKPASS` + entorno.
- Tests con `SDL_AUDIODRIVER=dummy` (silencio).
- Logs mínimos; no volcar logs enteros.
- Un cambio en `native/x86rt.h` ⇒ regenerar y recompilar los 85 `rc_*.c`.
- Antes de tocar algo dudoso, `git diff` para no pisar trabajo del usuario.
