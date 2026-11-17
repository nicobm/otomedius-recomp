# Otomedius (Arcade) — Handover para sesión nueva

> Pegar este documento entero como prompt inicial. El documento canónico y
> exhaustivo es `docs/BOOKLET.md`; **§13 manda sobre §1–§12**.
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
sh ../.work/tools/build.sh render.c d3dx9.c d3d9_objects.c xmodel.c

# regenerar el juego (SOLO si cambia native/x86rt.h; tarda y recompila 85 rc_*.c)
PYTHONPATH=../.work/pylib python3 tools/recomp/recomp.py \
    --exe ../.work/oto/otomedius.exe --out ../.work/gen --chunk 60

# test de humo reproducible (offscreen + audio dummy, 4700 presentes)
OTOMEDIUS_SMOKE_LOG=/tmp/opencode/smoke.log sh tests/smoke.sh

# corrida manual
cd ../.work/oto
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
  OTOMEDIUS_BOOTDBG=1 OTOMEDIUS_QUIT_AT=4400 ../obj/otomedius-recomp .
```

**Reglas de pruebas**
- `SDL_AUDIODRIVER=dummy` **siempre** en tests: si hay dispositivo, el mixer
  dsound→SDL suena de verdad.
- Logs grandes = tokens: `OTOMEDIUS_QUIT_AT` bajo, `grep`/`head`/`tail`.
- Dumps visuales: `OTOMEDIUS_DUMP_FB=/tmp/opencode/f.ppm` + `OTOMEDIUS_DUMP_AT=n`
  (o `OTOMEDIUS_DUMP_MODEL=/tmp/opencode/f.ppm` para el primer frame con
  modelo); convertir con `../.work/tools/ppm2png.py`.
- `native/x86rt.h` es el runtime: tocarlo obliga a regenerar y recompilar los 85
  `rc_*.c`.

---

## 2. Estado actual (verificado en esta sesión)

El juego arranca de punta a punta y **el smoke de 4700 pasa**:

`KonamiLogo` → `eAMUSELogo` → `Warning` → vídeo (~60 s) → `Title` →
eye-catch → **attract con `CGameMan` y state 5** → HUD 2D visible.

- `tests/smoke.sh`: **PASS** (`DEMO state=5`, `mask220=00000000`, sin
  `no recompiled code`, sin agotar thunks).
- **El `.x` real ya se parsea**: `native/xmodel.c` (+ `.h`) lee el binario
  (`xof 0303bin 0032`) que el juego pasa a
  `D3DXLoadMeshHierarchyFromXInMemory`: plantillas, listas de escalares
  compartidas, plantillas inline aplanadas, árbol de frames, mallas,
  materiales, normales y UVs. En proceso: `23 templates`, `58 frames,
  6 meshes, 7375 verts, 7998 faces`.
- **Jerarquía real**: `d3dx9.c` crea un frame del juego por frame del fichero
  (nombres, matrices, `+0x4c`/`+0x48`) y cuelga un contenedor nativo en cada
  frame con malla; `hle_frame_world()` compone las matrices y el `body` mide
  ≈11,6 u de alto, con `bike` debajo, `gunR`/`gunR1` a los costados y las
  manos delante.
- **Se dibuja**: `render_draw_model()` (WVP, z-buffer, luz direccional) pinta
  las 6 mallas (`model=6/7998` por frame).
- **Back buffer / render targets**: el framebuffer vive en memoria emulada,
  `GetBackBuffer` lo aliasea, `GetRenderTargetData`/`StretchRect` copian y
  `SetRenderTarget` redirige draws/clear a la textura.

Lo que falta: **que el modelo se vea**, la cámara real, las texturas del
modelo y el texto largo.

---

## 3. Qué se arregló en esta sesión (y no hay que volver a tocar)

1. **Cuelgue del attract en el present 4532** (`native/d3dx9.c`).
   `if (!is_player) RET32(0);` no retornaba (RET32 solo escribe `eax`), así
   que los ~80 `.x` no-player también construían 5 frames + contenedor; al
   cambiar de stage (`CPlayer02::Init`, `0x4ECBF3`) la máquina se quedaba
   girando (13 min de CPU sin log). Con el `return` y el parser real solo para
   el `.x` del jugador, el smoke de 4700 termina limpio.
2. **Parser del `.x` binario** (`native/xmodel.c`): ver §13.1 del booklet.
   Validado contra un prototipo Python independiente (frames, padres, mallas,
   materiales, primer vértice `2.309 0.170 3.132`, primera cara `3 2 6`).
3. **Jerarquía HLE real** en lugar de la nave sintética: `p02_handShape`,
   `p02_transparenceShape`, `p02_bodyShape`, `bikeShape`, `gunRShape`,
   `gunR1Shape`. El fallback sintético queda si el parseo falla.
4. **`ppFrame`/`ppAnim` y el contrato del allocator**: sigue igual que §12.3.
5. **Protocolo**: `INICIAR` ahora pide solo el token (cuenta/repo/email/rama
   se deducen del repo).

---

## 4. Bloqueo activo: el modelo se dibuja y se borra

Traza de orden (`OTOMEDIUS_ORDERDBG=1`) en **todos** los frames del attract:

```
CLEAR flags=7 color=ff000000      <- color + z + stencil a negro
MODEL p02_handShape … p02_transparenceShape   (6 mallas)
CLEAR flags=1 color=00000000      <- borra solo el color
UP  …                             <- HUD 2D
```

La escena 3D va a una **textura de escena** y se compone bajo el HUD. La
maquinaria ya está enganchada y **funciona**: la función de escena `0x450190`
hace `GetRenderTarget(0)` → `GetSurfaceLevel(0)` de `[0x5E1EB8]` →
`SetRenderTarget(0, surface)` → `call 0x415810` (recorrido de nodos, que llama
a `[container_vt+4]` desde `0x415846`; la malla `p02_transparenceShape` entra
por `0x4ECC31` de `CPlayer02`). `render_set_target()` redirige `Clear` y draws
al buffer de esa textura.

**Lo que falta comprobar**: en los dumps capturados el modelo no se ve, y hay
dos pistas fuertes:

1. la ventana en la que el attract dibuja el modelo es estrecha
   (≈present 4300–4440) y coincide con un *fade* (blanco → negro): dumpear en
   medio (`OTOMEDIUS_DUMP_FB` + `OTOMEDIUS_DUMP_EVERY=20` desde 4300);
2. el binding reporta una superficie de **256×256**, mientras el viewport del
   juego es 1280×1024: el rasterizador recorta al tamaño del target pero la
   proyección sigue siendo la del viewport grande, así que el modelo puede
   quedar fuera. Revisar `render_set_target`/`dev_SetViewport` para que el
   viewport acompañe al target (o componer con el tamaño correcto).

**Trampa ya pagada**: el primer test de render targets se hizo con un `d3d9.o`
viejo (faltaba en la lista de `build.sh`) y parecía que el juego no llamaba a
`SetRenderTarget`. Verificar siempre que el `.o` de la API tocada sea más
nuevo que su `.c`.

---

## 5. Plan inmediato (4 pasos)

1. **Visibilidad del modelo** (bloqueo): identificar el `ret=` del draw del
   contenedor y hacer que la escena llegue a la textura de escena (o al back
   buffer) *antes* del `Clear` del HUD.
2. **Cámara real**: capturar VIEW/PROJ del camino de shaders (matrices por
   handle de `ID3DXEffect`, hoy devuelve `0xe0000001` y guarda solo la última)
   y quitar el centrado de `render_draw_ship`/`render_draw_model`.
3. **Texturas del modelo**: los materiales son blancos; las texturas están en
   los `TXGP` de `Player02.pak`. Enlazarlas al contenedor HLE y muestrear con
   las UVs ya parseadas.
4. **HUD/texto y rendimiento**: corregir la línea larga del warning (§4.3),
   revisar blending y perfilar `draw_tri`/`tex_sample` con `model=6/7998`.

---

## 6. Reportes del usuario pendientes de diagnóstico

1. **Logos de Konami/eAMUSE y warning "warpeados"** (se ven y animan, pero
   deformados). Descartado: aspecto (el compositor usa escala uniforme
   `gs = min(...)` y el juego es 5:4 nativo, `CreateDevice(1280x1024)`) y
   pitch (`LockRect` devuelve `w*4`). Falta un dump del boot
   (`OTOMEDIUS_DUMP_FB` + `OTOMEDIUS_DUMP_AT` bajo, p. ej. 200/400/800) y
   comparar el glifo/logo contra la referencia. Ojo: el warning puede ser el
   bug conocido de texto largo (§4.3).
2. **Audio del vídeo "saltaba"**. `platform_audio_submit` descarta el chunk si
   la cola de SDL supera ~200 ms y el dispositivo no consume en 500 ms
   (`if (++spins > 100) return;`). Ahora loguea
   `audio: dropped N chunk(s), queue full`. Si aparece durante el vídeo, el
   problema es de *pacing* (frames largos por el rasterizador), no del
   decodificador; si no aparece, es underrun del dispositivo.

---

## 7. Direcciones y globals útiles

| Cosa | Valor |
|---|---|
| `CDemoPlayMan::Update` | `0x429340` (state en `this+0xA`) |
| `CGameMan::CGameMan` | `0x42E1F0`; slot `0x5DE260`; mask `0x5DE220` |
| `CDemoPlayMan` slot | `0x5DE258`; eye-catch `0x5DE25C` |
| `0x403DB0` | log + **pone `[0x5DE220] = 0xFFFFFFFF`** (suspende managers) |
| `CPlayer02` vtable / Init | `0x56CAFC` / `0x4ECBE0` (`stage obj` se imprime en `0x4ECBF3`) |
| `CModel` | 0x48 bytes; `+0` root frame, `+8` nodos (0x70), `+0x14` anim |
| `D3DXFRAME` layout | Name +0, matrix +4, pMeshContainer +0x44, sibling +0x48, child +0x4c |
| Loader `.x` | `0x414960`; allocator `0x559074`; `CreateFrame` `0x4167A0` |
| Textura de escena | `0x4505E0` (`CreateTexture` del tamaño del back buffer, `ppTexture` en `0x5E1EB8`) |
| `SetRenderTarget` (motor) | `0x415256`, `0x450215`, `0x450293` (slot `0x94`) |
| `ID3DXEffect::SetMatrix` | handle constante `0xe0000001`; falta mapear por handle |
| Back buffer → texture | `0x4505E0` (textura) + `GetRenderTargetData`/`StretchRect` |
| Device D3D9 | `[0x5E1F20]`; `g_xform` en `render.c` |
| Job list runner | `0x4095A0` (registros `{prio, fn, arg}` de 12 B) |

---

## 8. Herramientas y diagnóstico

- **REA** (MCP) está disponible. El provider Ghidra falló en sesiones
  anteriores, pero para leer una función concreta alcanza con el código
  recompilado: `../.work/gen/rc_*.c` (una función por `fn_XXXXXXXX`, con los
  comentarios de cada instrucción). Es la vía más rápida y exacta.
- `OTOMEDIUS_XMODEL_DBG=1`: plantillas, frames y mallas del `.x`.
- `OTOMEDIUS_ORDERDBG=1`: orden de envío (`CLEAR`/`UP`/`MODEL`).
- `OTOMEDIUS_DUMP_MODEL=<path.ppm>`: primer frame con modelo.
- `OTOMEDIUS_RTDBG=1`: `SetRenderTarget` / `GetRenderTargetData`.
- `OTOMEDIUS_3DDBG=1`: matrices de efecto, draws de la nave/modelo y
  `container draw ret=` (quién llama).
- `OTOMEDIUS_TEST_3D=1`: cubo XYZ de prueba.
- `OTOMEDIUS_SHIP_OVERLAY=1`: nave sintética encima de todo (diagnóstico).
- `OTOMEDIUS_NO_SHIP=1`: desactiva el HLE (vuelve al stub NULL).
- `OTOMEDIUS_WATCHDOG=1`, `OTOMEDIUS_TRIPWIRE=<addr>`, `OTOMEDIUS_TEXDUMP=WxH`,
  `OTOMEDIUS_TEXFMT=1`, `OTOMEDIUS_EFFECTDBG=1`, `OTOMEDIUS_MALLOCDBG=1`.

---

## 9. Reglas de la sesión

- Protocolo completo: `AGENTS.md` y `docs/BOOKLET.md` §0.
- No commits ni push sin `TERMINAR`.
- Nunca tokens en ficheros; `GIT_ASKPASS` + entorno.
- Tests con `SDL_AUDIODRIVER=dummy` (silencio).
- Logs mínimos; no volcar logs enteros.
- Un cambio en `native/x86rt.h` ⇒ regenerar y recompilar los 85 `rc_*.c`.
- Antes de tocar algo dudoso, `git diff` para no pisar trabajo del usuario.
