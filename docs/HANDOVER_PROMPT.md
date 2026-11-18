# Otomedius (Arcade) — Handover para sesión nueva

> Pegar este documento entero como prompt inicial. El documento canónico y
> exhaustivo es `docs/BOOKLET.md`; **§16 manda sobre §1–§15** en lo relativo al
> estado del port y al workflow de compilación.
> Las reglas de operación (`INICIAR` / `TERMINAR`) están en `AGENTS.md` y en
> `docs/BOOKLET.md` §0.
> Repo: `/var/home/vm/Proyectos/SALA DE TRABAJO/otomedius-recomp`.
> `INICIAR` alcanza con el **token** de GitHub: la cuenta, el repo, el email y
> la rama salen de `git remote`/`git config`.
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
| FreeType | 2.13.2 (`.work/freetype_include`, `.work/freetype_lib`) |

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

## 2. Granja de compilación (recomp-farm) — leer antes de compilar

El usuario mantiene una granja LAN fuera del repo:
`~/Proyectos/SALA DE TRABAJO/recomp-farm` (script `recomp-farm.sh`, un master en
esta VM y N workers). **El master no escucha**: sale él a los workers
(`tcp/47824`); su CLI necesita `podman`, que el sandbox **no tiene**.

El agente habla **TD1 directo contra los workers** con `tools/farm.py`, que lee
el secreto y la lista de workers del volumen del master
(`~/.local/share/containers/storage/volumes/recomp-farm-workspace/_data`).
No hay IPs en el código.

```sh
python3 tools/farm.py status                    # workers vivos + slots
python3 tools/farm.py scan                      # si la caché del master está vacía
python3 tools/farm.py sync ../.work/farm-build  # sube el kit, imprime el sha256
python3 tools/farm.py run --kit <sha> -- 'sh build-farm.sh shard-16.txt'
python3 tools/farm.py run --sync DIR --collect ../.work/farm-out -- '<cmd>'
```

Reglas:
- **Reproducibilidad obligatoria**: compilar en la granja con `-target
  x86_64-linux-gnu.2.44`, `-ffile-prefix-map=$PWD=.`,
  `-fdebug-compilation-dir=.` y `-ffile-prefix-map=$ZIGLIB=@zig` (receta en
  `README.md` §Build farm; el `build-farm.sh` del kit ya la genera en un
  `ccwrap`). Criterio: `sha256` de cada objeto **igual** al build local.
- **Un solo escritor por árbol de objetos**: los `.o` de la granja se linkean
  desde `.work/farm-out`, nunca se descomprimen sobre `.work/obj`.
- El kit del build completo está en `../.work/farm-build` (50 MB: `native/` +
  `gen/` + headers SDL3/FreeType + `build-farm.sh` + `shard-*.txt`).
- Verificado el 2026-11-17: 105/105 objetos idénticos y **binario idéntico**
  (`sha256 fb256033…`) con **4 nodos** (master 12 + 16 + 4 + 4 = 36 slots); el
  smoke pasa. El master también es nodo TD1 (`--no-master` para excluirlo).
- El usuario es quien hace `up`/`down` de la granja; si no hay workers, pedíselo
  en vez de intentar `podman`. **Antes de compilar, correr `farm.py status`**; si
  no responde, decirlo y seguir en local (no fingir que se usó la granja).

---

## 3. Estado actual (verificado)

El juego arranca de punta a punta y **el smoke de 4700 pasa**:

`KonamiLogo` → `eAMUSELogo` → `Warning` → vídeo (~60 s) → `Title` →
eye-catch → **attract con `CGameMan` y state 5** → HUD 2D → **modelo 3D del
jugador visible y texturado**.

- `tests/smoke.sh`: **PASS** (`DEMO state=5`, `mask220=00000000`, sin
  `no recompiled code`, sin agotar thunks).
- **El modelo se ve**: `render: frame 4320 … model=6/7998 clip=0 zout=0
  px_model>0`.
- **Cámara real**: `GetTransform(PROJECTION)` (0x414e96) y la matriz
  cámara→mundo del nodo (`nodo+0x30`, invertida).
- **Luz real**: el efecto `LightDir`/`LightCol` (handles distintos).
- **Texturado**: atlases `p02_body01`, `p02_bike01`, `p02_gun01`, `p02_hair`
  decodificados del pak en memoria emulada y enlazados por nombre de malla.
- **DXT1/3/5 corregido** (mezcla por canal).

Lo que falta: el mapeo material→textura real del motor, el escenario/enemigos,
`REPLAY RAND ERROR`, el texto largo y los logos del boot. El cartel de red ya no
aparece: `bms_ea3_get_status_wait` responde el estado "check finished" y el
juego llega a `[0x5de160]=5` (ver §3.1).

### 3.1 Cartel de red (resuelto en esta sesión)

El banner rojo `ネットワーク接続状況確認中` lo dibuja la capa 2D (`0x455bc0`)
mientras `[0x5de160] < 5`. La máquina de estados de red (`0x4296d0`) llama a
`bms_ea3_get_status_wait(int *status, int *sub)` y usa una tabla de saltos en
`0x429848` indexada por el estado:

| estado | salto | efecto |
|---|---|---|
| 0 | `0x429757` | `[0x5de160]=3`, `[0x5de164]=1`, reintenta |
| 1 | `0x429757` | igual que 0 (la tabla repite el destino) |
| 2 | `0x42977d` | mensaje del servidor pendiente → `[0x5de160]=2` o `4` |
| 3 | `0x4297ca` | `[0x5de160]=5` (**check terminado**) |

La tabla real en `0x429848` es `{0x429757, 0x429757, 0x42977d, 0x4297ca}`
(índice = valor de estado, `cmp esi,3; ja default`). El stub escribía 2 y el
juego se quedaba en 4; ahora escribe **3** en `*status` y 0 en `*sub`. `OTOMEDIUS_NETDBG=1` imprime cada llamada con el estado y los
globals. Verificado con dump de frame: el banner desaparece y el attract sigue.


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

1. ~~**Cartel de red**~~ **HECHO** (§3.1): `bms_ea3_get_status_wait` escribe
   `*status=3` y el juego llega a `[0x5de160]=5`; el banner ya no se dibuja.
2. **`REPLAY RAND ERROR`**: averiguar qué RNG compara el replay (el mensaje está
   en `0x55beb0`) y si el CRT recompilado usa la secuencia de MSVC.
3. **Material→textura** (ver §4) y blending/alpha del material
   (`p02_transparenceShape`).
4. **Texto largo del warning** y logos del boot (§4.3, §13.8 de `BOOKLET.md`).

Si el trabajo es de compilación masiva (regenerar + 85 `rc_*.c` + 20 nativos),
usar la granja (§2) y comparar `sha256` de los objetos con el build local.

---

## 6. Direcciones y globals útiles

| Cosa | Valor |
|---|---|
| `CDemoPlayMan::Update` | `0x429340` (state en `this+0xA`) |
| `CGameMan::CGameMan` | `0x42E1F0`; slot `0x5DE260`; mask `0x5DE220` |
| `CDemoPlayMan` slot | `0x5DE258`; eye-catch `0x5DE25C` |
| `CPlayer02` vtable / Init | `0x56CAFC` / `0x4ECBE0` (`stage obj` en `0x4ECBF3`) |
| `CModel` | 0x48 bytes; `+0` root frame, `+8` nodos (0x70), `+0x14` anim |
| `D3DXFRAME` layout | Name +0, matrix +4, pMeshContainer +0x44, sibling +0x48, child +0x4c |
| Loader `.x` | `0x414960`; allocator `0x559074`; `CreateFrame` `0x4167A0`; `CreateMeshContainer` `0x416810` |
| Contenedor draw / init / tex table | `0x416220` / `0x4163e0` / `0x416580` |
| Node walk | `0x415810` → `[container_vt+4](escena, nodo+0x30, nodo+0x20)` |
| Proyección | `D3DXMatrixPerspectiveFovLH` → `0x5e26f0`; fovy=1.7921, aspect=1.25, zn=30, zf=3000 |
| Efecto `SetMatrix` | `0x414ead` / `0x414edb` / `0x414ef2` / `0x414f09` |
| Device D3D9 | `[0x5E1F20]`; `g_xform` en `render.c` |
| Job list runner | `0x4095A0` (`{prio, fn, arg}` de 12 B) |
| Mensaje de red | `REPLAY RAND ERROR %d/%d` en `0x55beb0` |

---

## 7. Herramientas y diagnóstico

- **REA** (MCP) disponible. Para leer una función concreta alcanza con el código
  recompilado: `../.work/gen/rc_*.c` (una función por `fn_XXXXXXXX`).
- `tools/farm.py`: granja (§2).
- `OTOMEDIUS_3DDBG=1`, `ORDERDBG=<present>`, `RTDBG=1`, `VPDBG=1`, `TEXDBG=1`,
  `HARVESTDUMP=1`, `MODELTEX=<file.dds>`, `PAKDBG=<path>`, `PAKDUMP=<path>`,
  `PAKSCAN=<present>`, `EFFECTDUMP=<dir>`, `WATCHDOG=1`, `TRIPWIRE=<addr>`,
  `TEXDUMP=WxH`, `TEXFMT=1`, `DRAWDBG=<present>`, `MALLOCDBG=1`.
- Ficheros volcados útiles: `/tmp/opencode/tx4.ppm`, `/tmp/opencode/pak.x`,
  `/tmp/opencode/pakimg.bin`.

---

## 8. Reglas de la sesión

- Protocolo completo: `AGENTS.md` y `docs/BOOKLET.md` §0.
- No commits ni push sin `TERMINAR`.
- Nunca tokens en ficheros; `GIT_ASKPASS` + entorno.
- Tests con `SDL_AUDIODRIVER=dummy` (silencio).
- Logs mínimos; no volcar logs enteros.
- Un cambio en `native/x86rt.h` ⇒ regenerar y recompilar los 85 `rc_*.c`.
- Antes de tocar algo dudoso, `git diff` para no pisar trabajo del usuario.
- Granja: no correr `recomp-farm.sh` desde el sandbox; usar `tools/farm.py` y
  respetar la receta de reproducibilidad y el "un solo escritor por árbol".
