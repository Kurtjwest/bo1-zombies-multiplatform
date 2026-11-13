#!/usr/bin/env python3
"""LAN server for the browser build of BO1 Zombies (the Emscripten build, cmake/web.cmake).

The game runs in the browser, but its files stay on this machine: the page reads them from this server while it
plays (a byte range of a file at a time, as the game asks for it), so nothing is uploaded to or installed in the
browser. What the game writes into players/ (settings, profiles) is sent back here and kept in the saves folder, so
every device that plays from this server shares one set of settings.

    python3 tools/web-server.py                                  # the game folder of this machine's own build
    python3 tools/web-server.py --game "/path/to/Call of Duty Black Ops"
    python3 tools/web-server.py --port 8443 --site build-web/web

Then open https://<this machine's address>:8443/ on a device on the network (the server prints the address).

HTTPS: browsers only give a page the shared memory the game's threads need (SharedArrayBuffer, "cross-origin
isolation") on a secure origin: https://, or http://localhost on the machine itself. The server therefore makes its own
certificate authority and a certificate for this machine's names and addresses the first time it starts (with the
openssl command, kept in --state, by default ~/.bo1zombies-web). A browser warns about it until the authority is
trusted: open /ca.crt on the device and install it (iOS: Settings > General > VPN & Device Management, then Settings >
General > About > Certificate Trust Settings; Android: Settings > Security > Install a certificate > CA certificate;
macOS: Keychain Access, set it to Always Trust; Windows: certmgr.msc, Trusted Root Certification Authorities), or go
past the warning once per browser. --cert/--key use a certificate of your own (mkcert's, say); --http serves plain
HTTP, for localhost or behind a reverse proxy that adds HTTPS.

Behind a reverse proxy (nginx, say; see the browser section of README.md): run it with --http --bind 127.0.0.1 and
let the proxy pass a path (/bo1/, say) to it, its headers left as they are. --make-cert makes this server's certificate
for the proxy to use, if it has none of its own (--name adds the names devices reach the machine by).

This is for a home network: there are no accounts, and anyone who can reach the port can play and read the game
files. Don't forward it to the internet.

Python 3.8+ standard library only.
"""

import argparse
import email.utils
import errno
import hashlib
import http.server
import ipaddress
import json
import mimetypes
import os
import posixpath
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# folders below the game folder the game may write to through the server (kept in the saves folder, which overlays
# the game folder). Everything else the game writes (logs, the shader cache) stays in the browser's memory.
WRITABLE_PREFIXES = ("players/", "screenshots/")
MAX_UPLOAD = 64 << 20

# what the browser never needs from the game folder (the Windows executables and installers, Steam's files, and the
# logs a desktop build writes there: the browser's game writes its own, in its memory)
SKIP_EXT = {".exe", ".dll", ".pdb", ".bat", ".ps1", ".hta", ".url", ".lnk", ".msi", ".cab", ".vdf", ".log"}
SKIP_DIRS = {"redist", "_commonredist", "directx", "$tempdir", ".git", "shadercache", "__pycache__"}


def default_game_dirs():
    home = os.path.expanduser("~")
    dirs = []
    if sys.platform == "darwin":
        dirs.append(os.path.join(home, "Library", "Application Support", "BO1Zombies"))
    xdg = os.environ.get("XDG_DATA_HOME") or os.path.join(home, ".local", "share")
    dirs.append(os.path.join(xdg, "BO1Zombies"))
    return dirs


def looks_like_game_dir(d):
    try:
        names = {n.lower() for n in os.listdir(d)}
    except OSError:
        return False
    return "zone" in names and "main" in names


class FileTree:
    """The game folder as the browser sees it: the game's files, the build's own (mods/, main/ from the repository)
    over them, and the saved files over both. Paths are relative, with '/', in the spelling of the folder that has
    the file; lookups ignore case, as the game's do on Windows."""

    def __init__(self, layers):
        self.layers = layers  # [(name, root, prefix in the tree)] lowest first
        self.lock = threading.Lock()
        self.files = {}  # lower-case path -> (path, size, mtime, full path)
        self.manifest = b""
        self.etag = ""
        self.scanned = 0.0

    def scan(self):
        files = {}
        for _, root, prefix in self.layers:
            if not os.path.isdir(root):
                continue
            for dirpath, dirnames, filenames in os.walk(root, followlinks=True):
                dirnames[:] = [d for d in dirnames if d.lower() not in SKIP_DIRS and not d.startswith(".")]
                rel_dir = os.path.relpath(dirpath, root).replace(os.sep, "/")
                rel_dir = prefix + ("" if rel_dir == "." else rel_dir + "/")
                for name in filenames:
                    if name.startswith(".") or os.path.splitext(name)[1].lower() in SKIP_EXT:
                        continue
                    full = os.path.join(dirpath, name)
                    try:
                        st = os.stat(full)
                    except OSError:
                        continue
                    rel = rel_dir + name
                    low = rel.lower()
                    prev = files.get(low)
                    # (a file of an upper layer keeps the spelling the lower one had: one file, not two)
                    files[low] = (prev[0] if prev else rel, st.st_size, int(st.st_mtime), full)
        entries = sorted((v[0], v[1], v[2]) for v in files.values())
        body = json.dumps({"version": 1, "files": entries}, separators=(",", ":")).encode()
        with self.lock:
            self.files = files
            self.manifest = body
            self.etag = '"%s"' % hashlib.sha1(body).hexdigest()[:20]
            self.scanned = time.time()
        return len(entries), sum(e[1] for e in entries)

    def lookup(self, rel):
        with self.lock:
            return self.files.get(rel.lower())

    def update_saved(self, rel, full):
        """a saved file changed (or was removed: full None)"""
        low = rel.lower()
        with self.lock:
            if full is None:
                self.files.pop(low, None)
            else:
                st = os.stat(full)
                prev = self.files.get(low)
                self.files[low] = (prev[0] if prev else rel, st.st_size, int(st.st_mtime), full)
            entries = sorted((v[0], v[1], v[2]) for v in self.files.values())
            self.manifest = json.dumps({"version": 1, "files": entries}, separators=(",", ":")).encode()
            self.etag = '"%s"' % hashlib.sha1(self.manifest).hexdigest()[:20]


def clean_rel(path):
    """a request path below a root as a relative path, or None when it leaves it"""
    path = urllib.parse.unquote(path)
    if "\0" in path or "\\" in path:
        return None
    norm = posixpath.normpath("/" + path).lstrip("/")
    if norm in ("", ".") or norm.startswith("../") or norm == "..":
        return None
    return norm


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "BO1ZombiesWeb/1"
    protocol_version = "HTTP/1.1"

    # (set on the class by main())
    tree = None
    site = None
    saves = None
    ca_cert = None
    quiet = True

    def log_message(self, fmt, *args):
        if not self.quiet:
            sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))

    def log_error(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))

    def isolation_headers(self):
        # cross-origin isolation (SharedArrayBuffer for the game's threads)
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")

    def send_plain(self, code, text):
        body = (text + "\n").encode()
        self.send_response(code)
        self.isolation_headers()
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def send_bytes(self, body, ctype, etag=None, cache="no-cache"):
        if etag and self.headers.get("If-None-Match") == etag:
            self.send_response(304)
            self.isolation_headers()
            self.send_header("ETag", etag)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_response(200)
        self.isolation_headers()
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", cache)
        if etag:
            self.send_header("ETag", etag)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def send_file(self, full, ctype=None, cache="no-cache"):
        try:
            f = open(full, "rb")
        except OSError:
            self.send_plain(404, "not found")
            return
        with f:
            st = os.fstat(f.fileno())
            size = st.st_size
            etag = '"%x-%x"' % (int(st.st_mtime), size)
            ctype = ctype or mimetypes.guess_type(full)[0] or "application/octet-stream"
            start, end = 0, size - 1
            status = 200
            rng = self.headers.get("Range")
            if rng and rng.startswith("bytes=") and "," not in rng and self.headers.get("If-Range", etag) == etag:
                a, _, b = rng[6:].strip().partition("-")
                try:
                    if a == "":
                        start, end = max(size - int(b), 0), size - 1
                    else:
                        start = int(a)
                        end = min(int(b), size - 1) if b else size - 1
                except ValueError:
                    start, end = 0, size - 1
                if start >= size or start > end:
                    self.send_response(416)
                    self.isolation_headers()
                    self.send_header("Content-Range", "bytes */%d" % size)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                status = 206
            elif self.headers.get("If-None-Match") == etag:
                self.send_response(304)
                self.isolation_headers()
                self.send_header("ETag", etag)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            length = end - start + 1
            self.send_response(status)
            self.isolation_headers()
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(length))
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("ETag", etag)
            self.send_header("Last-Modified", email.utils.formatdate(st.st_mtime, usegmt=True))
            self.send_header("Cache-Control", cache)
            if status == 206:
                self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
            self.end_headers()
            if self.command == "HEAD":
                return
            f.seek(start)
            left = length
            while left > 0:
                chunk = f.read(min(left, 1 << 20))
                if not chunk:
                    break
                self.wfile.write(chunk)
                left -= len(chunk)

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        path = url.path
        if path == "/files/manifest.json":
            # (rescanned when asked to, or when it is a minute old: files added to the game folder appear on reload)
            if "rescan" in url.query or time.time() - self.tree.scanned > 60:
                self.tree.scan()
            with self.tree.lock:
                body, etag = self.tree.manifest, self.tree.etag
            self.send_bytes(body, "application/json", etag)
            return
        if path.startswith("/files/"):
            rel = clean_rel(path[len("/files/"):])
            hit = self.tree.lookup(rel) if rel else None
            if not hit:
                self.send_plain(404, "not found")
                return
            self.send_file(hit[3], "application/octet-stream")
            return
        if path == "/ca.crt" and self.ca_cert:
            with open(self.ca_cert, "rb") as f:
                self.send_bytes(f.read(), "application/x-x509-ca-cert")
            return
        rel = clean_rel(path) if path != "/" else "index.html"
        if rel is None:
            self.send_plain(404, "not found")
            return
        full = os.path.join(self.site, *rel.split("/"))
        if os.path.isdir(full):
            full = os.path.join(full, "index.html")
        if not os.path.isfile(full):
            self.send_plain(404, "not found")
            return
        ctype = None
        if full.endswith(".wasm"):
            ctype = "application/wasm"
        elif full.endswith(".js") or full.endswith(".mjs"):
            ctype = "text/javascript"
        self.send_file(full, ctype)

    def writable(self, rel):
        return rel and any(rel.lower().startswith(p) for p in WRITABLE_PREFIXES) and self.saves

    def do_PUT(self):
        url = urllib.parse.urlsplit(self.path)
        rel = clean_rel(url.path[len("/files/"):]) if url.path.startswith("/files/") else None
        if not self.writable(rel):
            self.send_plain(403, "the game may only write players/ and screenshots/ here")
            return
        try:
            length = int(self.headers.get("Content-Length", "-1"))
        except ValueError:
            length = -1
        if length < 0 or length > MAX_UPLOAD:
            self.send_plain(413, "too large")
            return
        data = self.rfile.read(length)
        # (in the spelling the game folder has, when it has the file)
        hit = self.tree.lookup(rel)
        rel_spelled = hit[0] if hit else rel
        full = os.path.join(self.saves, *rel_spelled.split("/"))
        os.makedirs(os.path.dirname(full), exist_ok=True)
        fd, tmp = tempfile.mkstemp(dir=os.path.dirname(full), prefix=".upload-")
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.replace(tmp, full)
        self.tree.update_saved(rel_spelled, full)
        self.send_plain(200, "saved")

    def do_DELETE(self):
        url = urllib.parse.urlsplit(self.path)
        rel = clean_rel(url.path[len("/files/"):]) if url.path.startswith("/files/") else None
        if not self.writable(rel):
            self.send_plain(403, "the game may only write players/ and screenshots/ here")
            return
        hit = self.tree.lookup(rel)
        if hit and os.path.abspath(hit[3]).startswith(os.path.abspath(self.saves) + os.sep):
            try:
                os.remove(hit[3])
            except OSError:
                pass
            self.tree.update_saved(hit[0], None)
        self.send_plain(200, "removed")


def lan_addresses():
    addrs = set()
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))  # (no packet is sent: picks the interface of the default route)
        addrs.add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            addrs.add(info[4][0])
    except OSError:
        pass
    addrs.discard("0.0.0.0")
    return sorted(a for a in addrs if not a.startswith("127."))


def make_certificates(state, extra_names=()):
    """this server's own certificate authority and a certificate signed by it for this machine's names and addresses,
    and any other names given (made again when they change)"""
    ca_key = os.path.join(state, "ca-key.pem")
    ca_crt = os.path.join(state, "ca.crt")
    key = os.path.join(state, "server-key.pem")
    crt = os.path.join(state, "server.crt")
    names_file = os.path.join(state, "server-names.txt")
    host = socket.gethostname()
    names = ["localhost", host]
    if not host.endswith(".local"):
        names.append(host.split(".")[0] + ".local")
    names += list(extra_names)
    ips = ["127.0.0.1"] + lan_addresses()
    san = ",".join(["DNS:" + n for n in dict.fromkeys(names)] + ["IP:" + i for i in dict.fromkeys(ips)])
    have = open(names_file).read() if os.path.exists(names_file) else ""
    if os.path.exists(crt) and os.path.exists(key) and os.path.exists(ca_crt) and have == san:
        return ca_crt, crt, key

    def run(args, **kw):
        subprocess.run(["openssl"] + args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, **kw)

    try:
        if not (os.path.exists(ca_key) and os.path.exists(ca_crt)):
            run(["req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", ca_key, "-out", ca_crt, "-days", "3650",
                 "-subj", "/CN=BO1 Zombies LAN server (%s)" % host,
                 "-addext", "basicConstraints=critical,CA:TRUE",
                 "-addext", "keyUsage=critical,keyCertSign,cRLSign"])
        csr = os.path.join(state, "server.csr")
        ext = os.path.join(state, "server.ext")
        run(["req", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", csr, "-subj", "/CN=%s" % host])
        with open(ext, "w") as f:
            f.write("basicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\n"
                    "extendedKeyUsage=serverAuth\nsubjectAltName=%s\n" % san)
        # (825 days: the longest Apple's systems accept for a server certificate)
        run(["x509", "-req", "-in", csr, "-CA", ca_crt, "-CAkey", ca_key, "-CAcreateserial", "-out", crt,
             "-days", "825", "-sha256", "-extfile", ext])
        os.remove(csr)
        os.remove(ext)
        with open(names_file, "w") as f:
            f.write(san)
        os.chmod(ca_key, 0o600)
        os.chmod(key, 0o600)
    except (OSError, subprocess.CalledProcessError) as e:
        detail = e.stderr.decode(errors="replace").strip() if getattr(e, "stderr", None) else str(e)
        sys.exit("Could not make a certificate with openssl (%s). Install openssl, pass --cert and --key, or use "
                 "--http (localhost only)." % detail)
    return ca_crt, crt, key


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, client_address):
        e = sys.exc_info()[1]
        if isinstance(e, (ConnectionError, ssl.SSLError, socket.timeout)) or (
                isinstance(e, OSError) and e.errno in (errno.EPIPE, errno.ECONNRESET)):
            return  # (a browser that dropped the connection, or refused the certificate)
        super().handle_error(request, client_address)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", help="the game folder (zone/ and main/ of a Black Ops PC install); default: this "
                                   "machine's BO1Zombies game folder")
    ap.add_argument("--site", default=None, help="the browser build (default: build-web/web in the repository)")
    ap.add_argument("--gamedata", default=None,
                    help="the build's own game files, over the game folder's (default: build-web/gamedata, else the "
                         "repository's mods/ and data/main/)")
    ap.add_argument("--state", default=os.path.join(os.path.expanduser("~"), ".bo1zombies-web"),
                    help="where the certificates and saved files are kept (default: ~/.bo1zombies-web)")
    ap.add_argument("--bind", default="0.0.0.0", help="address to listen on (default: all)")
    ap.add_argument("--port", type=int, default=8443)
    ap.add_argument("--http", action="store_true", help="plain HTTP (works from http://localhost only)")
    ap.add_argument("--cert", help="certificate (PEM) instead of the server's own")
    ap.add_argument("--key", help="its private key (PEM)")
    ap.add_argument("--name", action="append", default=[],
                    help="another name of this machine for its certificate (as often as needed): the name devices "
                         "reach it by, if not its own host name")
    ap.add_argument("--make-cert", action="store_true",
                    help="make the certificate authority and this machine's certificate in --state, print where they "
                         "are and quit (for a reverse proxy, such as nginx, that serves the game over HTTPS)")
    ap.add_argument("--verbose", action="store_true", help="log every request")
    args = ap.parse_args()

    if args.make_cert:
        state = os.path.abspath(os.path.expanduser(args.state))
        os.makedirs(state, exist_ok=True)
        ca_crt, crt, key = make_certificates(state, args.name)
        print("certificate:     %s\nits key:         %s\nauthority:       %s (for the devices: <address>/ca.crt)"
              % (crt, key, ca_crt))
        print("names:           %s" % open(os.path.join(state, "server-names.txt")).read())
        return

    game = args.game
    if game:
        game = os.path.abspath(os.path.expanduser(game))
    else:
        game = next((d for d in default_game_dirs() if looks_like_game_dir(d)), None)
        if not game:
            sys.exit("No game folder found in %s: pass --game with the folder of a Black Ops PC install."
                     % " or ".join(default_game_dirs()))
    if not looks_like_game_dir(game):
        sys.exit("%s has no zone/ and main/ folders: not a Black Ops game folder." % game)

    site = os.path.abspath(args.site or os.path.join(REPO, "build-web", "web"))
    if not os.path.isfile(os.path.join(site, "index.html")):
        sys.exit("No browser build in %s: build it first (./build-web.sh, see README.md), or pass --site." % site)

    state = os.path.abspath(os.path.expanduser(args.state))
    saves = os.path.join(state, "saves")
    os.makedirs(saves, exist_ok=True)

    tree_layers = [("game", game, "")]
    gamedata = os.path.abspath(args.gamedata or os.path.join(os.path.dirname(site), "gamedata"))
    if os.path.isdir(gamedata):
        tree_layers.append(("the build's files", gamedata, ""))
    else:
        # the repository's own files: mods/ -> mods/, data/main/ -> main/
        for name, src in (("mods", os.path.join(REPO, "mods")), ("main", os.path.join(REPO, "data", "main"))):
            if os.path.isdir(src):
                tree_layers.append(("the repository's " + name, src, name + "/"))
    tree_layers.append(("saved files", saves, ""))
    tree = FileTree(tree_layers)
    count, total = tree.scan()

    Handler.tree = tree
    Handler.site = site
    Handler.saves = saves
    Handler.quiet = not args.verbose

    httpd = Server((args.bind, args.port), Handler)
    scheme = "http"
    if not args.http:
        if args.cert and args.key:
            crt, key = args.cert, args.key
        else:
            Handler.ca_cert, crt, key = make_certificates(state, args.name)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(crt, key)
        httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
        scheme = "https"

    print("BO1 Zombies web server")
    print("  game files: %s (%d files, %.1f GB)" % (game, count, total / 1e9))
    for name, root, _ in tree_layers[1:]:
        print("  over them:  %s (%s)" % (root, name))
    print("  the game:   %s" % site)
    print("Open on a device on this network:")
    hosts = ["localhost"] + lan_addresses() if args.bind in ("0.0.0.0", "") else [args.bind]
    for h in hosts:
        print("  %s://%s:%d/" % (scheme, h, args.port))
    if Handler.ca_cert:
        print("Certificate authority to install on the devices (removes the browser's warning): /ca.crt (%s)"
              % Handler.ca_cert)
    if args.http:
        # (behind a reverse proxy with the certificate --make-cert made, the devices still find its authority here)
        if os.path.isfile(os.path.join(state, "ca.crt")):
            Handler.ca_cert = os.path.join(state, "ca.crt")
        print("(plain HTTP: browsers allow the game's threads on http://localhost only, or behind a reverse proxy that "
              "adds HTTPS)")
    sys.stdout.flush()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
