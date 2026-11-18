#!/usr/bin/env python3
"""Cliente de red para la granja `recomp-farm` (protocolo TD1 v3 + HAVE).

    python3 tools/farm.py status                 # workers vivos (caché del master + PING)
    python3 tools/farm.py scan                   # forzar un escaneo de la LAN
    python3 tools/farm.py sync DIR               # subir el kit, imprime el sha256 canónico
    python3 tools/farm.py run --kit <sha> -- 'sh build.sh'      # sin transmitir nada
    python3 tools/farm.py run --sync DIR -- 'sh build.sh'       # HAVE/NEED, no retransmite
    python3 tools/farm.py run -- 'uname -a; nproc'

Por qué existe: el master de la granja **no escucha en ningún puerto** — es un
cliente que sale a los workers (`socat TCP4-LISTEN:47824` en cada worker). Los
jobs se disparan con `recomp-farm.sh`, que necesita `podman`, y el agente corre
dentro de un sandbox Flatpak sin `podman` y sin acceso al host. Lo que el
sandbox sí ve es el volumen del master, y lo que el protocolo pide es un HMAC
con el secreto compartido que vive ahí.

Así que este cliente hace lo mismo que hace el master, sin podman:

  1. lee `.config/secret` y `.config/slaves{,.cache}` del volumen del master,
  2. comprueba cada worker con `TD1 PING` (sin auth, como el escaneo del master),
  3. pregunta `TD1 HAVE <sha>` antes de mandar un kit (cache content-addressed),
  4. manda `TD1 JOB` firmado con HMAC-SHA256 y devuelve stdout + exit code.

No hay ninguna IP en el código: las direcciones salen de `slaves`/`slaves.cache`
(que escribe el master al escanear) o de `--subnet`/`scan` explícitos. No se
escribe nada en el volumen del master y el secreto nunca se imprime.

El kit se empaqueta con la **misma receta determinística** que
`recomp-farm.sh` (`tar_kit`), así el sha256 coincide con el que imprime
`./recomp-farm.sh sync` y el caché HAVE de los workers se aprovecha entre
cliente y CLI:

    tar --sort=name --owner=0 --group=0 --numeric-owner -cf - . | gzip -n

Variables: OTOMEDIUS_FARM_VOL (volumen del master), OTOMEDIUS_FARM_PORT (47824).

Protocolo (v3; ver README de recomp-farm, "Protocolo TD1")::

    TD1 PING <nonce>                -> TD1 PONG <host> <slots> <port> <ver> <user> <role>
    TD1 HAVE <sha256>               -> TD1 HAVE <sha256> | TD1 NEED <sha256>
                                       | TD1 ERR bad request   (worker viejo)
    TD1 JOB <nonce> <ts> <auth> <id> <tmo> <b64cwd> <cmdlen> <syncsize> <synchash>
        <cmd: cmdlen bytes><tar: syncsize bytes>
                                    -> TD1 NEEDSECRET / TD1 SECRET <s>
                                    -> TD1 OK <nonce> <ts> <auth>   (ACEPTADO, sigue corriendo)
                                       <stdout+stderr en vivo>
                                       TD1 DONE <nonce> <rc>
    auth = HMAC-SHA256(secreto, "v1\\njob\\n<nonce>\\n<ts>\\n<id>\\n<tmo>\\n<b64cwd>
                                \\n<cmdlen>\\n<syncsize>\\n<synchash>")

Con `syncsize > 0` el worker extrae el tar en `/workspace/jobs/<synchash>` y
corre ahí. Con `syncsize=0` + `synchash` usa el kit ya cacheado (si no está,
`TD1 ERR missing kit` → reintentamos mandando el tar). `{dir}` lo reemplaza el
worker con su rundir real.
"""

import argparse
import base64
import concurrent.futures
import hashlib
import hmac
import os
import secrets
import socket
import subprocess
import sys
import tarfile
import tempfile
import time

DEFAULT_VOL = os.path.expanduser(
    "~/.local/share/containers/storage/volumes/recomp-farm-workspace/_data"
)
VOL = os.environ.get("OTOMEDIUS_FARM_VOL", DEFAULT_VOL)
PORT = int(os.environ.get("OTOMEDIUS_FARM_PORT", "47824"))
BUNDLE_TAG = "TDFARM-BUNDLE:"
# Subredes que prueba el master cuando no hay caché (mismas que recomp-farm.sh).
DEFAULT_SUBNETS = ("192.168.0", "192.168.1", "192.168.2")

CONFIG = os.path.join(VOL, ".config")


# --------------------------------------------------------------------------
# estado de la granja (volumen del master)
# --------------------------------------------------------------------------
def farm_secret():
    """Secreto compartido HMAC. Se usa, no se imprime."""
    try:
        with open(os.path.join(CONFIG, "secret"), encoding="utf-8") as fh:
            return fh.read().strip() or None
    except OSError:
        return None


def cache_entries():
    """(addr, origen) de .config/slaves y .config/slaves.cache."""
    out = []
    for name in ("slaves", "slaves.cache"):
        try:
            with open(os.path.join(CONFIG, name), encoding="utf-8") as fh:
                items = fh.read().split()
        except OSError:
            continue
        for item in items:
            if item and not item.startswith("#"):
                out.append((item, name))
    return out


def parse_addr(item):
    host, _, port = item.partition(":")
    return host, int(port) if port.isdigit() else PORT


def local_ip():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect(("10.0.2.2", 80))
        return sock.getsockname()[0]
    except OSError:
        return None
    finally:
        sock.close()


# --------------------------------------------------------------------------
# descubrimiento
# --------------------------------------------------------------------------
def _read_line(sock):
    """Lee una línea (bytes) con buffer de a un byte: sólo para cabeceras."""
    data = b""
    while b"\n" not in data:
        chunk = sock.recv(512)
        if not chunk:
            break
        data += chunk
    return data.split(b"\n", 1)[0].decode("utf-8", "replace").strip()


def ping(item, timeout=2.0):
    """TD1 PING: el worker contesta host, slots, puerto, versión, usuario, rol."""
    host, port = parse_addr(item)
    try:
        with socket.create_connection((host, port), timeout) as sock:
            sock.settimeout(timeout)
            sock.sendall(b"TD1 PING %d\n" % os.getpid())
            line = _read_line(sock)
    except OSError:
        return None
    if not line.startswith("TD1 PONG "):
        return None
    fields = line.split()[2:]
    return {
        "addr": host,
        "port": port,
        "host": fields[0] if len(fields) > 0 else host,
        "slots": fields[1] if len(fields) > 1 else "?",
        "version": fields[3] if len(fields) > 3 else "?",
        "user": fields[4] if len(fields) > 4 else "-",
        "role": fields[5] if len(fields) > 5 else "-",
    }


def scan_subnets(subnets, timeout=2.0, parallel=128):
    candidates = []
    for net in subnets:
        base = net.rstrip(".")
        candidates.extend("%s.%d" % (base, last) for last in range(1, 255))
    me = local_ip()
    candidates = [c for c in candidates if c != me]
    found = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=parallel) as pool:
        futures = {pool.submit(ping, c, timeout): c for c in candidates}
        for fut in concurrent.futures.as_completed(futures):
            info = fut.result()
            if info:
                found.append(info)
    return found


# El master también acepta jobs TD1 (role=master+worker) y, como el sandbox
# comparte su namespace de red, se lo alcanza por loopback.
MASTER_ADDR = "127.0.0.1"


def pick_workers(args):
    """Nodos vivos: master en loopback + caché del master + (si hace falta) scan."""
    entries = cache_entries()
    workers = []
    seen = set()

    def add(info, source):
        key = (info["addr"], info["port"])
        if key not in seen:
            seen.add(key)
            info["source"] = source
            workers.append(info)

    if not getattr(args, "no_master", False):
        info = ping("%s:%d" % (MASTER_ADDR, PORT), args.ping_timeout)
        if info:
            add(info, "master")

    from_cache = 0
    for item, origin in entries:
        info = ping(item, args.ping_timeout)
        if info:
            add(info, origin)
            from_cache += 1
    # la presencia del master no reemplaza al scan: si la caché no dio nada,
    # seguimos y escaneamos la LAN
    if from_cache:
        return workers
    if entries and not getattr(args, "scan", False):
        print(
            "[farm] la caché del master no responde (%d entrada/s); probá: farm.py scan"
            % len(entries),
            file=sys.stderr,
        )
        return workers  # al menos el master, si está

    subnets = list(getattr(args, "subnet", None) or []) or list(DEFAULT_SUBNETS)
    print(
        "[farm] escaneando %s en tcp/%d"
        % (", ".join(s + ".0/24" for s in subnets), PORT),
        file=sys.stderr,
    )
    for info in scan_subnets(subnets, args.ping_timeout):
        add(info, "scan")
    return workers


# --------------------------------------------------------------------------
# kits (misma receta determinística que recomp-farm.sh:tar_kit)
# --------------------------------------------------------------------------
class Kit:
    """tar.gz determinístico en un fichero temporal + su sha256 canónico."""

    def __init__(self, directory):
        self.dir = directory
        fd, self.path = tempfile.mkstemp(prefix="farm-kit-", suffix=".tar.gz")
        os.close(fd)
        self.size = 0
        self.sha = ""
        self._build()

    def _build(self):
        recipes = (
            ["tar", "--sort=name", "--owner=0", "--group=0", "--numeric-owner",
             "-cf", "-", "."],
            ["tar", "-cf", "-", "."],  # fallback si el tar no soporta las flags
        )
        for tar_cmd in recipes:
            ok = _pipe_gzip(tar_cmd, self.dir, self.path)
            if ok:
                self.size = os.path.getsize(self.path)
                digest = hashlib.sha256()
                with open(self.path, "rb") as fh:
                    for chunk in iter(lambda: fh.read(1 << 20), b""):
                        digest.update(chunk)
                self.sha = digest.hexdigest()
                return
        raise RuntimeError("no pude empaquetar %s" % self.dir)

    def remote_dir(self):
        return "/workspace/jobs/" + self.sha

    def cleanup(self):
        try:
            os.unlink(self.path)
        except OSError:
            pass


def _pipe_gzip(tar_cmd, directory, out_path):
    """tar | gzip -n > out_path, en streaming (kits de cientos de MB)."""
    try:
        with open(out_path, "wb") as out:
            tar = subprocess.Popen(
                tar_cmd, cwd=directory, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL
            )
            gz = subprocess.Popen(
                ["gzip", "-n"], stdin=tar.stdout, stdout=out, stderr=subprocess.DEVNULL
            )
            tar.stdout.close()
            rc_gz = gz.wait()
            rc_tar = tar.wait()
    except OSError:
        return False
    return rc_tar == 0 and rc_gz == 0 and os.path.getsize(out_path) > 0


def kit_state(worker, sha, timeout=4.0):
    """TD1 HAVE <sha> -> 'have' | 'need' | 'old' (worker sin soporte HAVE)."""
    try:
        with socket.create_connection((worker["addr"], worker["port"]), timeout) as sock:
            sock.settimeout(timeout)
            sock.sendall(("TD1 HAVE %s\n" % sha).encode())
            line = _read_line(sock)
    except OSError:
        return "old"
    if line.startswith("TD1 HAVE"):
        return "have"
    if line.startswith("TD1 NEED"):
        return "need"
    return "old"


# --------------------------------------------------------------------------
# protocolo (lado cliente, igual que run_remote en recomp-farm.sh)
# --------------------------------------------------------------------------
def job_auth(secret, nonce, ts, jid, tmo, b64cwd, cmdlen, syncsize, synchash):
    data = "\n".join(
        ["v1", "job", nonce, str(ts), jid, str(tmo), b64cwd,
         str(cmdlen), str(syncsize), synchash]
    )
    return hmac.new(secret.encode(), data.encode(), hashlib.sha256).hexdigest()


class _Lines:
    """Lector de líneas con buffer propio (OK + salida en vivo + DONE)."""

    def __init__(self, sock):
        self.sock = sock
        self.buf = b""

    def line(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                return None
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return line.decode("utf-8", "replace")


def _send_file(sock, path):
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            sock.sendall(chunk)


def _attempt(worker, command, secret, cwd, timeout, kit, send_tar, on_line):
    start = time.time()
    nonce = secrets.token_hex(16)
    ts = int(time.time())
    jid = "%d-%d" % (os.getpid(), secrets.randbelow(100000))
    cmd = command.encode()
    b64cwd = base64.b64encode(cwd.encode()).decode()
    syncsize = kit.size if (send_tar and kit) else 0
    synchash = kit.sha if kit else ""
    auth = job_auth(secret, nonce, ts, jid, timeout, b64cwd, len(cmd), syncsize, synchash)
    header = "TD1 JOB %s %d %s %s %d %s %d %d %s\n" % (
        nonce, ts, auth, jid, timeout, b64cwd, len(cmd), syncsize, synchash,
    )

    sock = socket.create_connection((worker["addr"], worker["port"]), timeout=10)
    sock.settimeout(timeout + 120)
    try:
        sock.sendall(header.encode())
        sock.sendall(cmd)
        if syncsize:
            _send_file(sock, kit.path)
        reader = _Lines(sock)
        line = reader.line()
        if line == "TD1 NEEDSECRET":
            sock.sendall(b"TD1 SECRET " + secret.encode() + b"\n")
            line = reader.line()
        if line is None:
            return 1, "(el worker cerró la conexión)\n", time.time() - start, None
        if not line.startswith("TD1 OK"):
            return 1, line + "\n", time.time() - start, line
        out = []
        while True:
            line = reader.line()
            if line is None:
                return 1, "".join(out), time.time() - start, None
            if line.startswith("TD1 DONE " + nonce + " "):
                try:
                    rc = int(line.rsplit(" ", 1)[1])
                except ValueError:
                    rc = 255
                return rc, "".join(out), time.time() - start, None
            out.append(line + "\n")
            if on_line:
                on_line(worker["label"], line)
    finally:
        sock.close()


def run_job(worker, command, secret, cwd="", timeout=900, kit=None,
            send_tar=True, on_line=None):
    """Manda un TD1 JOB. Devuelve (rc, salida, segundos, error_protocolo)."""
    rc, out, secs, err = _attempt(
        worker, command, secret, cwd, timeout, kit, send_tar, on_line
    )
    # El kit cacheado desapareció entre HAVE y JOB: reintentamos con el tar.
    if err and "missing kit" in err and kit and not send_tar:
        rc, out, secs, err = _attempt(
            worker, command, secret, cwd, timeout, kit, True, on_line
        )
    return rc, out, secs, err


# --------------------------------------------------------------------------
# comandos
# --------------------------------------------------------------------------
def _label_workers(workers):
    """Dos máquinas pueden llamarse igual (p. ej. dos "fedora")."""
    repeats = {}
    for w in workers:
        repeats[w["host"]] = repeats.get(w["host"], 0) + 1
    for w in workers:
        w["label"] = (
            w["host"] if repeats[w["host"]] == 1
            else "%s-%s" % (w["host"], w["addr"].replace(".", "-"))
        )


def render(command, worker, total_slots):
    """Placeholders del master; {dir} se deja al worker (conoce su rundir)."""
    return (
        command.replace("{host}", worker["host"])
        .replace("{slots}", str(worker["slots"]))
        .replace("{total_slots}", str(total_slots))
        .replace("{user}", str(worker.get("user", "")))
    )


def save_bundle(directory, host, out):
    """Extrae el tar.gz+base64 que el job marcó con TDFARM-BUNDLE:."""
    blob = None
    for line in out.splitlines():
        if line.startswith(BUNDLE_TAG):
            blob = line[len(BUNDLE_TAG):]
            break
    if not blob:
        return None
    try:
        raw = base64.b64decode(blob)
    except ValueError as exc:
        print("[farm] bundle ilegible de %s: %s" % (host, exc), file=sys.stderr)
        return None
    dest = os.path.join(directory, host)
    os.makedirs(dest, exist_ok=True)
    tmp = os.path.join(directory, ".%s.tar" % host)
    with open(tmp, "wb") as fh:
        fh.write(raw)
    try:
        with tarfile.open(tmp) as tar:
            try:
                tar.extractall(dest, filter="data")
            except TypeError:  # Python < 3.12
                tar.extractall(dest)
    finally:
        os.unlink(tmp)
    return dest


def cmd_status(args):
    if not os.path.isdir(CONFIG):
        print("no encuentro el volumen del master: %s" % VOL, file=sys.stderr)
        print("(¿está corriendo `bash recomp-farm.sh` en el host?)", file=sys.stderr)
        return 2
    secret = farm_secret()
    workers = pick_workers(args)
    if not workers:
        print("no hay workers vivos (¿`recomp-farm.sh` en las otras máquinas?)")
        entries = cache_entries()
        print(
            "caché del master: %s"
            % (", ".join(a for a, _ in entries) if entries else "(vacía)")
        )
        return 1
    _label_workers(workers)
    print("%-16s %-6s %-22s %-6s %-8s %-14s %s"
          % ("ADDR", "SLOTS", "HOST", "PORT", "VERSION", "ROLE", "USER"))
    for w in sorted(workers, key=lambda x: x["label"]):
        print(
            "%-16s %-6s %-22s %-6d %-8s %-14s %s"
            % (w["addr"], w["slots"], w["label"], w["port"], w["version"],
               w.get("role", "-"), w["user"])
        )
    print(
        "total %d slots en %d worker(s); secreto %s"
        % (
            sum(int(w["slots"]) for w in workers if str(w["slots"]).isdigit()),
            len(workers),
            "presente" if secret else "FALTA",
        )
    )
    return 0


def cmd_scan(args):
    args.scan = True
    return cmd_status(args)


def cmd_sync(args):
    if not os.path.isdir(args.directory):
        print("no es un directorio: %s" % args.directory, file=sys.stderr)
        return 2
    kit = Kit(args.directory)
    try:
        print(
            "[farm] kit %s: %d KiB, sha256 %s"
            % (args.directory, kit.size // 1024, kit.sha),
            file=sys.stderr,
        )
        args.command = ["true"]
        args.kit_sha = kit.sha
        return dispatch(args, kit)
    finally:
        kit.cleanup()


def cmd_run(args):
    if not args.command:
        print("falta el comando: farm.py run -- <comando>", file=sys.stderr)
        return 2
    kit = None
    try:
        if args.sync and args.kit_sha:
            print("usá --sync DIR o --kit SHA, no los dos", file=sys.stderr)
            return 2
        if args.sync:
            if not os.path.isdir(args.sync):
                print("--sync no es un directorio: %s" % args.sync, file=sys.stderr)
                return 2
            kit = Kit(args.sync)
            args.kit_sha = kit.sha
            print(
                "[farm] kit %s: %d KiB, sha256 %s"
                % (args.sync, kit.size // 1024, kit.sha),
                file=sys.stderr,
            )
        elif args.kit_sha:
            args.kit_sha = args.kit_sha.strip().lower()
            if len(args.kit_sha) != 64 or any(c not in "0123456789abcdef" for c in args.kit_sha):
                print("--kit espera un sha256 de 64 hex", file=sys.stderr)
                return 2
        return dispatch(args, kit)
    finally:
        if kit:
            kit.cleanup()


def dispatch(args, kit):
    secret = farm_secret()
    if not secret:
        print("falta %s/.config/secret (¿arrancaste el master?)" % VOL, file=sys.stderr)
        return 2
    workers = pick_workers(args)
    if getattr(args, "hosts", None):
        wanted = {h for h in args.hosts.replace(",", " ").split() if h}
        workers = [w for w in workers if w["host"] in wanted or w["addr"] in wanted]
    if not workers:
        print("no hay workers vivos ni coinciden con --hosts", file=sys.stderr)
        return 2
    _label_workers(workers)

    kit_sha = getattr(args, "kit_sha", None)
    command = " ".join(args.command)
    cwd = "/workspace/jobs/" + kit_sha if kit_sha else (getattr(args, "cwd", None) or "/workspace")
    total_slots = sum(int(w["slots"]) for w in workers if str(w["slots"]).isdigit())

    # ¿quién ya tiene el kit? (HAVE; los workers viejos responden ERR y reciben tar)
    send_tar = {}
    for w in workers:
        if not kit_sha:
            send_tar[w["label"]] = False
            continue
        state = kit_state(w, kit_sha)
        if state == "have":
            send_tar[w["label"]] = False
            print("[farm] %s: kit en caché" % w["label"], file=sys.stderr)
        elif state == "need":
            if kit is None:
                print(
                    "[farm] %s no tiene el kit %s…: subilo con `farm.py sync DIR`"
                    % (w["label"], kit_sha[:16]),
                    file=sys.stderr,
                )
                return 2
            send_tar[w["label"]] = True
            print(
                "[farm] %s: kit %d KiB a enviar" % (w["label"], kit.size // 1024),
                file=sys.stderr,
            )
        else:  # worker v3 viejo: sin HAVE, mandamos el tar
            if kit is None:
                print(
                    "[farm] %s no soporta HAVE y no tengo el tar; subilo con `farm.py sync DIR`"
                    % w["label"],
                    file=sys.stderr,
                )
                return 2
            send_tar[w["label"]] = True
            print("[farm] %s: worker sin HAVE, envío el kit" % w["label"], file=sys.stderr)

    print(
        "[farm] %d worker(s), timeout %ds, cwd %s"
        % (len(workers), args.timeout, cwd),
        file=sys.stderr,
    )

    stream = getattr(args, "stream", False)
    on_line = (lambda label, line: print("[%s] %s" % (label, line), flush=True)) if stream else None

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(workers)) as pool:
        futures = {
            pool.submit(
                run_job, w, render(command, w, total_slots), secret, cwd,
                args.timeout, kit, send_tar[w["label"]], on_line,
            ): w
            for w in workers
        }
        for fut in concurrent.futures.as_completed(futures):
            worker = futures[fut]
            try:
                rc, out, secs, _err = fut.result()
            except OSError as exc:
                rc, out, secs = 1, "(error de red: %s)\n" % exc, 0.0
            results.append((worker, rc, out, secs))

    failed = 0
    for worker, rc, out, secs in sorted(results, key=lambda r: r[0]["label"]):
        if rc != 0:
            failed += 1
        collect = getattr(args, "collect", None)
        saved = None
        # --collect corre también con --quiet: callar la salida no debe tirar
        # los objetos (antes el `continue` se los comía).
        if collect and BUNDLE_TAG in out:
            saved = save_bundle(collect, worker["label"], out)
        if getattr(args, "quiet", False) and rc == 0:
            if saved:
                print("[farm] objetos de %s en %s" % (worker["label"], saved))
            continue
        print("\n===== %s exit=%d (%.1fs) =====" % (worker["label"], rc, secs))
        if collect:
            out = "".join(
                line + "\n" for line in out.splitlines()
                if not line.startswith(BUNDLE_TAG)
            )
        if not stream:  # con --stream ya se imprimió en vivo
            sys.stdout.write(out)
            if out and not out.endswith("\n"):
                print()
        if saved:
            print("[farm] objetos de %s en %s" % (worker["label"], saved))

    if kit_sha:
        print(
            "[farm] kit sha256 %s  (reusar: --kit %s)" % (kit_sha, kit_sha),
            file=sys.stderr,
        )
    print(
        "\nsummary: %d ok, %d failed, %d worker(s)"
        % (len(results) - failed, failed, len(results)),
        file=sys.stderr,
    )
    return 0 if failed == 0 else 1


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Cliente de red para la granja recomp-farm (sin podman).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("--ping-timeout", type=float, default=2.0,
                    help="timeout de TD1 PING (default 2s)")
    ap.add_argument("--subnet", action="append", default=[],
                    help="subred /24 a escanear, p. ej. 192.168.5 (repetible)")
    ap.add_argument("--no-master", action="store_true",
                    help="no incluir el master (127.0.0.1) como nodo")
    sub = ap.add_subparsers(dest="cmd", required=True)

    st = sub.add_parser("status", help="workers vivos")
    st.add_argument("--scan", action="store_true", help="escanear aunque haya caché")
    st.set_defaults(func=cmd_status)

    sc = sub.add_parser("scan", help="forzar escaneo de la LAN")
    sc.set_defaults(func=cmd_scan)

    ru = sub.add_parser("run", help="mandar un comando a los workers")
    ru.add_argument("--sync", metavar="DIR",
                    help="empaquetar y subir DIR (pregunta HAVE antes)")
    ru.add_argument("--kit", dest="kit_sha", metavar="SHA256",
                    help="usar un kit ya subido (no transmite nada)")
    ru.add_argument("--cwd", help="directorio remoto (default: el del kit o /workspace)")
    ru.add_argument("--timeout", type=int, default=900)
    ru.add_argument("--hosts", help="sólo estos workers (nombre o IP)")
    ru.add_argument("--collect", metavar="DIR",
                    help="extraer los tar.gz+base64 marcados con " + BUNDLE_TAG)
    ru.add_argument("--stream", action="store_true",
                    help="imprimir la salida en vivo con prefijo [host]")
    ru.add_argument("--quiet", action="store_true", help="sólo los que fallan")
    ru.add_argument("command", nargs=argparse.REMAINDER)
    ru.set_defaults(func=cmd_run)

    sy = sub.add_parser("sync", help="subir un kit y decir su sha256")
    sy.add_argument("directory")
    sy.add_argument("--hosts", help="sólo estos workers")
    sy.add_argument("--timeout", type=int, default=900)
    sy.set_defaults(func=cmd_sync)

    args = ap.parse_args(argv)
    if args.cmd == "run" and args.command and args.command[0] == "--":
        args.command = args.command[1:]
    for name, value in (
        ("scan", False), ("subnet", []), ("cwd", None), ("timeout", 900),
        ("hosts", None), ("quiet", False), ("collect", None), ("stream", False),
        ("sync", None), ("kit_sha", None), ("command", []),
    ):
        if not hasattr(args, name):
            setattr(args, name, value)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
