# AGENTS.md — reglas de operación del port de Otomedius

Este repositorio es la recompilación estática AOT de **Otomedius (Konami,
2008)** a Linux x86-64, empaquetada como Flatpak. **No contiene datos del
juego**: el ejecutable original se traduce en la máquina del usuario a partir
de su propio dump.

La referencia técnica canónica y acumulativa es **`docs/BOOKLET.md`**. El plan
de la sesión en curso está en **`docs/HANDOVER_PROMPT.md`**. Si hay conflicto,
manda la sección más reciente del booklet (hoy §16) y, después, el prompt.

---

## 0. Rol y método

Actuás como **ingeniero senior de ingeniería inversa y de sistemas en C**,
especializado en recompilación estática de binario a C. El objetivo es mantener
**paridad lógica exacta** con el binario original de forma quirúrgica, eficiente
y limpia.

### 0.1 Reglas críticas

1. **Nada de fuerza bruta.** No se corre el juego a ciegas ni se repiten
   ejecuciones para "adivinar" fallos leyendo logs. Cada corrida debe tener una
   hipótesis concreta y una variable de diagnóstico acotada (`OTOMEDIUS_QUIT_AT`
   bajo, `OTOMEDIUS_TRIPWIRE`, `OTOMEDIUS_WATCH`, `OTOMEDIUS_DUMP_FB`). Los
   `grep`/`head`/`tail` sobre logs sirven para **verificar** una hipótesis
   concreta, no para explorar a ciegas.
2. **Cero parches masivos.** Nada de reescribir archivos o bloques enteros.
   Cambios atómicos, quirúrgicos y bien delimitados; el diff debe explicar por sí
   solo qué se corrigió y por qué.
3. **Una sola fuente de verdad.** Antes de tocar código, mirar el binario
   original: REA/Ghidra (MCP) si está disponible y, si no, el código recompilado
   `../.work/gen/rc_*.c` (una función por `fn_XXXXXXXX`). No se deduce la
   semántica del original a partir de logs de la recompilación.
4. **Documentar el workaround** en `native/patches.c` y en `docs/BOOKLET.md`.

### 0.2 Flujo obligatorio ante una discrepancia

1. **Análisis estático primero.** Consultar la firma, los tipos, los offsets y
   el grafo de control en el binario original (REA/Ghidra o `rc_*.c`).
   Confirmar qué hace realmente la función, no qué parece hacer.
2. **Parche quirúrgico.** Editar solo las líneas necesarias para alinear la
   lógica o los tipos; mantener el estilo del fichero.
3. **Verificación aislada.** Antes de levantar el juego entero, compilar el
   fichero (`sh ../.work/tools/build.sh <fichero>.c`) y, si aplica, un test
   mínimo o `-fsyntax-only`.
4. **Validación.** Recién entonces correr el smoke o una corrida acotada con
   audio dummy. Si el cambio toca `native/x86rt.h`, regenerar y recompilar todo.
5. **Compilación distribuida.** Usar la granja (§6) para builds completos o de
   varios ficheros; para un solo fichero, el build local es más rápido. Si la
   granja no responde, decirlo y seguir en local; **nunca** afirmar que se usó
   la granja si no se usó.

---

## 1. Señales de sesión

### `INICIAR`

El usuario envía la palabra **`INICIAR`** acompañada solo del **token de
GitHub**. La cuenta, el repositorio, el email y la rama se deducen del propio
árbol de trabajo; no hace falta repetirlos:

| Dato | De dónde se deduce |
|---|---|
| cuenta / repositorio | `git remote get-url origin` |
| email | `git config user.email` (o el `Author` de `git log -1`) |
| rama de push | `git branch --show-current` (`master` por defecto) |

Si el usuario los menciona igualmente, se usan como comprobación cruzada: si no
coinciden con el remoto configurado, se avisa antes de tocar nada.

Con esa señal, la IA debe:

1. Leer `AGENTS.md`, la sección más reciente de `docs/BOOKLET.md` y
   `docs/HANDOVER_PROMPT.md` completos antes de tocar nada.
2. Ejecutar `git log -1` y `git status` y respetar cualquier cambio sin
   commitear que encuentre (puede ser trabajo del usuario).
3. Verificar el estado: compilar solo los ficheros tocados y correr el smoke
   test (ver §3) antes de cambiar código.
4. Continuar el plan de `docs/HANDOVER_PROMPT.md` §5.

### `TERMINAR`

El usuario envía la palabra **`TERMINAR`**. Solo entonces la IA puede:

1. Correr los tests completos (`tests/smoke.sh`, siempre con audio dummy).
2. Actualizar `README.md`, `docs/BOOKLET.md` (nueva sección de sesión al final,
   con estado verificado, cambios por fichero y bloqueos) y
   `docs/HANDOVER_PROMPT.md` (prompt de la sesión siguiente).
3. Hacer `git add`, un commit descriptivo y `git push origin master`.
4. Informar el hash del commit y el resultado del push.

**Sin `TERMINAR` (o permiso explícito del usuario) no se hace commit ni push.**
No se reescribe historia ni se hace `force-push`.

### Token de GitHub

El token llega **solo en el mensaje `INICIAR`** (o cuando se pida `TERMINAR`).
Se mantiene únicamente en la memoria de la sesión: **nunca** se escribe en un
fichero del repo, ni en `AGENTS.md`, ni en un comando que quede visible en logs.
Para el push se usa una variable de entorno y un `GIT_ASKPASS` temporal fuera
del repositorio:

```sh
# /tmp/opencode/askpass.sh solo imprime la variable; no contiene el token
printf '#!/bin/sh\nprintf "%%s\\n" "$GITHUB_TOKEN"\n' > /tmp/opencode/askpass.sh
chmod +x /tmp/opencode/askpass.sh
GITHUB_TOKEN=... GIT_ASKPASS=/tmp/opencode/askpass.sh \
  GIT_TERMINAL_PROMPT=0 git push origin master
```

El token se pasa por el entorno de ese único comando y no se guarda.

---

## 2. Entorno

| Item | Valor |
|---|---|
| Host / target | Linux x86-64 (sin Windows/macOS/32-bit) |
| Compilador local | `zig cc` 0.17.1 (`../.work/zig/zig`) |
| Compilador Flatpak | SDK `cc` (GCC) + `pkg-config sdl3 freetype2` |
| UI | SDL3 3.4.20, renderer OpenGL |
| Recompilador | Python 3 + capstone 5.0.9 (`tools/recomp`) |
| Directorio de trabajo | raíz del repo; `.work` está en el directorio padre |

Comandos habituales:

```sh
# compilar la capa nativa (solo los ficheros tocados) y enlazar
sh ../.work/tools/build.sh render.c d3dx9.c patches.c font.c win32_misc.c

# regenerar el juego SOLO si cambia native/x86rt.h (tarda y recompila 85 rc_*.c)
PYTHONPATH=../.work/pylib python3 tools/recomp/recomp.py \
    --exe ../.work/oto/otomedius.exe --out ../.work/gen --chunk 60

# smoke test reproducible (offscreen + audio dummy, 4700 presentes)
OTOMEDIUS_SMOKE_LOG=/tmp/opencode/smoke.log sh tests/smoke.sh

# corrida manual
cd ../.work/oto
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy \
  OTOMEDIUS_BOOTDBG=1 OTOMEDIUS_QUIT_AT=4700 ../obj/otomedius-recomp .
```

---

## 3. Reglas de pruebas y diagnóstico

- **Siempre** `SDL_AUDIODRIVER=dummy` en tests: si hay dispositivo, el mixer
  dsound→SDL suena de verdad.
- Logs grandes = tokens. Usar `OTOMEDIUS_QUIT_AT` bajo y `grep`/`head`/`tail`
  **sobre una hipótesis concreta**; no volcar logs enteros al contexto ni correr
  el juego a ciegas (ver §0.1).
- Dumps visuales: `OTOMEDIUS_DUMP_FB=/tmp/opencode/frame.ppm` +
  `OTOMEDIUS_DUMP_AT=n`; convertir con `../.work/tools/ppm2png.py`.
- Variables útiles: `OTOMEDIUS_BOOTDBG`, `OTOMEDIUS_DRAWDBG`, `OTOMEDIUS_3DDBG`,
  `OTOMEDIUS_TEST_3D`, `OTOMEDIUS_SHIP_OVERLAY`, `OTOMEDIUS_TEXDUMP`,
  `OTOMEDIUS_WATCHDOG`, `OTOMEDIUS_TRIPWIRE`.
- `tests/smoke.sh` exige: exit 0, sin `no recompiled code`, `DEMO state=5`,
  `CGameMan ctor returned … mask220=00000000` y que no se agote la tabla de
  thunks.

---

## 4. Reglas de código

- Mantener el estilo y la estructura de los ficheros existentes.
- Los cambios de esta capa nativa son solo `native/*.c` / `native/*.h`; el
  código del juego se regenera con `tools/recomp`.
- **`native/x86rt.h` es el runtime compartido**: cualquier cambio obliga a
  regenerar y recompilar **todos** los objetos, incluidos los 85 `rc_*.c`.
  Mezclar objetos viejos produce corrupción silenciosa.
- No añadir datos del juego (paks, ejecutables, texturas, fuentes) al repo.
- Antes de borrar o reescribir algo dudoso, `git diff` y `git status` para no
  pisar trabajo del usuario.
- Documentar cada workaround en `native/patches.c` y en el booklet.

---

## 5. Estado de referencia

- El juego arranca de punta a punta: loader → boot Konami → NVRAM → logos →
  Warning → vídeo → Title → eye-catch → attract con `CGameMan` y state 5.
- El HUD 2D se dibuja y **el modelo 3D del jugador se ve y está texturado**:
  `.x` real parseado (`native/xmodel.c`), cámara y luz de la escena, atlases
  `p02_*.dds` decodificados desde la imagen del pak en memoria.
- Falta el escenario/enemigos (store por modelo), el mapeo material→textura del
  motor y el texto largo. El cartel de red ya no aparece: `bms_ea3_get_status_wait`
  responde el estado "check finished" y el juego llega a `[0x5de160]=5`.
- Los bloqueos y el plan inmediato están en `docs/HANDOVER_PROMPT.md` §4–§5 y
  en la última sección de `docs/BOOKLET.md` (§16).

---

## 6. Granja de compilación (recomp-farm)

El usuario mantiene fuera del repo la granja **recomp-farm** (hoy en
`~/Proyectos/SALA DE TRABAJO/recomp-farm`): un **master** en esta VM (dentro del
NAT de Boxes) y N **workers** de la LAN, todo podman rootless, con imagen
**Arch** (`zig 0.17.1`, gcc, distcc) y sin acceso a este filesystem. El master
**sale** a los workers (`tcp/47824`) y además **escucha él mismo**
(`role=master+worker`): sus núcleos también toman shard, alcanzables por
loopback desde el sandbox.

| Quién | Qué hace |
|---|---|
| usuario | ciclo de vida: `./recomp-farm.sh up|down|scan|status|logs` |
| IA | `python3 tools/farm.py status|sync|run` — manda compilaciones y tests, lee logs y objetos |

Reglas:

- **La IA no corre `recomp-farm.sh`** (es `podman exec`, y en el sandbox no hay
  podman). El enlace es TD1 contra los workers, firmado con el HMAC del secreto
  que vive en el volumen `recomp-farm-workspace/.config/secret`. `tools/farm.py`
  implementa ese cliente; no hay IPs en el código (salen de `slaves{,.cache}` o
  de un `scan`/`--subnet` explícito).
- El secreto no se imprime ni se copia, y no se escribe nada en el volumen del
  master.
- **Kits**: `farm.py sync DIR` imprime el `sha256`; el tar es determinístico
  (`tar --sort=name --owner=0 --group=0 --numeric-owner -cf - . | gzip -n`), así
  el cache `TD1 HAVE` pega entre el CLI y el agente. `--kit <sha>` no transmite
  nada; con un worker viejo (sin HAVE) hay que mandar el tar.
- **Reproducibilidad**: compilar en la granja siempre con `-target
  x86_64-linux-gnu.2.44`, `-ffile-prefix-map=$PWD=.`,
  `-fdebug-compilation-dir=.` y `-ffile-prefix-map=$ZIGLIB=@zig`
  (receta completa en `README.md` §Build farm). Criterio de aceptación:
  `sha256` de cada objeto igual al del build local; si no coincide, no se usa.
- El dump **no** va a la granja: `--sync` manda sólo lo que el job necesita.
- **Un solo escritor por árbol de objetos**: los `.o` de la granja se linkean
  desde un directorio aparte (`.work/farm-out`); nunca se descomprimen sobre
  `.work/obj`.
- Verificado 2026-11-17: 105/105 objetos idénticos y **binario idéntico**
  (`sha256 fb256033…`) con 4 nodos (master 12 + 16 + 4 + 4 = **36 slots**); el
  smoke pasa con ese binario.
- `tools/farm.py` incluye al master como candidato en `127.0.0.1` (se puede
  excluir con `--no-master`); el reparto se hace con `--hosts <addr>` y listas
  por shard.
- **Antes de compilar, comprobar la granja**: `python3 tools/farm.py status`.
  Si no hay workers vivos, avisar al usuario y compilar en local; no quedarse
  esperando ni decir que se usó la granja. El usuario es quien hace `up`/`down`
  (`recomp-farm.sh`); la IA no puede (no hay `podman` en el sandbox).

---

## 7. Atribución

La recompilación estática, la ingeniería inversa y la capa nativa Linux se
desarrollan con varios LLMs usados a través de **OpenCode** como agente de
código.
