#!/usr/bin/env python3
"""Live Silo + P2Play MediaMesh acceptance test.

Builds the pinned Silo server, installs the real plugin through Silo's plugin
manager, discovers/subscribes to a P2Play-published list, searches it, adds a
release through the native Silo torrent engine, and reads real torrent bytes
from Silo's authenticated stream API.

Requires: Go 1.26+, Python libtorrent, git, and a built p2playd.
The MediaMesh network is a three-node local test mesh. The actual media torrent
uses a local HTTP tracker and a loopback libtorrent seeder; no public tracker is
used.
"""
import argparse
import hashlib
import http.server
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
import io

import libtorrent as lt

PIN = "3610db846fd2132f06aa626a4cb94f4a0cfa69ae"
UPSTREAM = "https://github.com/YouROK/TorrServer.git"
OWNER_PASSWORD = "silo-live-test-owner-password"
P2P_TOKEN = "ts_silolive00000000000000000000"
HERE = Path(__file__).resolve().parent.parent
PLUGIN_DIR = HERE / "plugins" / "mediamesh"
P2P_NODES = ("publisher", "silo-node", "second-holder")


def check(condition, message):
    if not condition:
        raise AssertionError(message)
    print("OK   " + message, flush=True)


def wait_for(label, predicate, seconds=60):
    deadline = time.monotonic() + seconds
    last_error = None
    while time.monotonic() < deadline:
        try:
            value = predicate()
            if value:
                return value
        except Exception as exc:
            last_error = exc
        time.sleep(0.25)
    suffix = f"; last error: {last_error}" if last_error else ""
    raise TimeoutError(f"timed out waiting for {label}{suffix}")


def free_port(used):
    for _ in range(100):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        if port not in used:
            used.add(port)
            return port
    raise RuntimeError("could not allocate a unique loopback port")


def local_ip():
    # DHT implementations reject loopback peers. This selects the runner's
    # local interface address without sending a packet to the documentation IP.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.connect(("192.0.2.1", 9))
        address = sock.getsockname()[0]
    if address.startswith("127."):
        raise RuntimeError("runner has no non-loopback interface for the local DHT mesh")
    return address


def http_request(base, path, method="GET", body=None, raw=None, headers=None,
                 bearer=None, cookie=None, timeout=10):
    payload = raw if raw is not None else (
        None if body is None else json.dumps(body).encode("utf-8")
    )
    request = urllib.request.Request(base + path, data=payload, method=method)
    supplied = dict(headers or {})
    if bearer:
        supplied["Authorization"] = "Bearer " + bearer
    if cookie:
        supplied["Cookie"] = "silo_token=" + cookie
    if body is not None:
        supplied.setdefault("Content-Type", "application/json")
    for name, value in supplied.items():
        request.add_header(name, value)
    try:
        response = urllib.request.urlopen(request, timeout=timeout)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        data = response.read()
        content_type = response.headers.get("Content-Type", "")
        if data and "application/json" in content_type:
            try:
                data = json.loads(data)
            except ValueError:
                pass
        return response.status, data, dict(response.headers.items())


class P2PNode:
    def __init__(self, daemon, root, name, http_port, peer_port, bootstrap, catalog_only=False):
        self.daemon = daemon
        self.root = Path(root) / name
        self.name = name
        self.http_port = http_port
        self.peer_port = peer_port
        self.proc = None
        self.log = None
        self.root.mkdir(parents=True)
        settings = {
            "PeersListenPort": peer_port,
            "P2PDhtBootstrap": bootstrap,
        }
        if catalog_only:
            settings["P2PCatalogOnly"] = True
        (self.root / "settings.json").write_text(json.dumps(settings), encoding="utf-8")
        (self.root / "auth.json").write_text(json.dumps({
            "users": [{
                "username": "owner",
                "password": OWNER_PASSWORD,
                "api_token": P2P_TOKEN,
            }]
        }), encoding="utf-8")

    @property
    def base(self):
        return f"http://127.0.0.1:{self.http_port}"

    def call(self, path, method="GET", body=None, raw=None, timeout=10):
        return http_request(self.base, path, method=method, body=body, raw=raw,
                            bearer=P2P_TOKEN, timeout=timeout)

    def start(self):
        self.log = (self.root / "stdout.log").open("ab")
        self.proc = subprocess.Popen(
            [self.daemon, "--data", str(self.root), "--port", str(self.http_port)],
            stdout=self.log, stderr=subprocess.STDOUT, start_new_session=True,
        )
        wait_for(self.name + " p2playd startup", self.ready, 25)
        check(True, self.name + " P2Play daemon starts")

    def ready(self):
        if self.proc.poll() is not None:
            raise RuntimeError(self.read_log()[-4000:])
        status, _, _ = self.call("/api/system/ping", timeout=2)
        return status == 200

    def read_log(self):
        path = self.root / "stdout.log"
        return path.read_text(errors="replace") if path.exists() else ""

    def stop(self):
        if self.proc is not None and self.proc.poll() is None:
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                self.proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(self.proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                self.proc.wait(timeout=5)
        if self.log and not self.log.closed:
            self.log.close()


def bencode(value):
    if isinstance(value, int):
        return b"i" + str(value).encode("ascii") + b"e"
    if isinstance(value, bytes):
        return str(len(value)).encode("ascii") + b":" + value
    if isinstance(value, list):
        return b"l" + b"".join(bencode(item) for item in value) + b"e"
    if isinstance(value, dict):
        return b"d" + b"".join(
            bencode(key) + bencode(value[key]) for key in sorted(value)
        ) + b"e"
    raise TypeError("unsupported bencode value")


class LocalTracker:
    def __init__(self, seed_port):
        self.seed_port = seed_port
        self.announces = 0
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                if urllib.parse.urlsplit(self.path).path != "/announce":
                    self.send_error(404)
                    return
                owner.announces += 1
                peers = socket.inet_aton("127.0.0.1") + struct.pack("!H", owner.seed_port)
                body = bencode({
                    b"complete": 1,
                    b"incomplete": 0,
                    b"interval": 5,
                    b"peers": peers,
                })
                self.send_response(200)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                try:
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    # A libtorrent client may close an announce socket as soon
                    # as its timeout/response processing is complete.
                    pass

            def log_message(self, *_args):
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.port = self.server.server_address[1]
        self.url = f"http://127.0.0.1:{self.port}/announce"
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)


class Seeder:
    def __init__(self, torrent_info, save_path, port):
        self.session = lt.session({
            "listen_interfaces": f"127.0.0.1:{port}",
            "enable_dht": False,
            "enable_lsd": False,
            "enable_upnp": False,
            "enable_natpmp": False,
            "allow_multiple_connections_per_ip": True,
        })
        self.handle = self.session.add_torrent({
            "ti": torrent_info,
            "save_path": str(save_path),
        })
        wait_for("local libtorrent seeder", lambda: (
            self.handle.status().state == lt.torrent_status.seeding
        ), 15)
        self.port = port

    def close(self):
        try:
            self.session.pause()
        except Exception:
            pass
        self.handle = None
        self.session = None


class SiloServer:
    def __init__(self, binary, root, port):
        self.binary = binary
        self.root = Path(root)
        self.port = port
        self.base = f"http://127.0.0.1:{port}"
        self.data = self.root / "silo-data"
        self.plugins = self.root / "silo-plugins"
        self.data.mkdir(parents=True)
        self.plugins.mkdir(parents=True)
        self.config = self.root / "config.yaml"
        self.config.write_text(
            "server:\n"
            "  host: \"127.0.0.1\"\n"
            f"  port: {port}\n"
            "storage:\n"
            f"  data_dir: {json.dumps(str(self.data))}\n"
            "  db_name: \"torrserver.db\"\n"
            "plugins:\n"
            f"  dir: {json.dumps(str(self.plugins))}\n"
            "auth:\n"
            f"  owner_password: {json.dumps(OWNER_PASSWORD)}\n"
            "logging:\n"
            "  level: \"info\"\n",
            encoding="utf-8",
        )
        self.proc = None
        self.log = None
        self.token = None

    def call(self, path, method="GET", body=None, raw=None, headers=None, timeout=10):
        return http_request(self.base, path, method=method, body=body, raw=raw,
                            headers=headers, bearer=self.token, cookie=self.token,
                            timeout=timeout)

    def start(self):
        self.log = (self.root / "silo.log").open("ab")
        self.proc = subprocess.Popen(
            [self.binary, "-c", str(self.config)],
            stdout=self.log, stderr=subprocess.STDOUT, start_new_session=True,
        )
        wait_for("Silo server startup", self.ready, 30)
        check(True, "pinned Silo server starts")

    def ready(self):
        if self.proc.poll() is not None:
            raise RuntimeError(self.read_log()[-5000:])
        status, _, _ = http_request(self.base, "/api/system/ping", timeout=2)
        return status == 200

    def login(self):
        status, data, _ = http_request(
            self.base, "/api/auth/login", method="POST",
            body={"username": "owner", "password": OWNER_PASSWORD},
        )
        check(status == 200 and isinstance(data, dict) and data.get("token"),
              "Silo owner login")
        self.token = data["token"]

    def read_log(self):
        path = self.root / "silo.log"
        return path.read_text(errors="replace") if path.exists() else ""

    def stop(self):
        if self.proc is not None and self.proc.poll() is None:
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                self.proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(self.proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                self.proc.wait(timeout=5)
        if self.log and not self.log.closed:
            self.log.close()

    def set_isolated_torrent_config(self):
        status, config, _ = self.call("/api/system/torrent-config")
        check(status == 200 and isinstance(config, dict), "read Silo torrent config")
        config["disable_dht"] = True
        config["disable_pex"] = True
        config["disable_upnp"] = True
        config["disable_utp"] = True
        config["disable_tcp"] = False
        config["enable_ipv6"] = False
        config["listen_port"] = 0
        status, result, _ = self.call("/api/system/torrent-config", method="POST", body=config)
        check(status == 200 and result.get("restart_required") is True,
              "disable public DHT/PEX/UPnP for the Silo media fixture")
        self.stop()
        self.start()
        self.login()

    def upload_plugin(self):
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as bundle:
            for name in ("info.yaml", "service.js", "index.html", "app.js", "style.css"):
                bundle.write(PLUGIN_DIR / name, name)
        boundary = "----P2PlaySiloLiveBoundary"
        body = (
            b"--" + boundary.encode() + b"\r\n"
            b'Content-Disposition: form-data; name="plugin"; filename="mediamesh.zip"\r\n'
            b"Content-Type: application/zip\r\n\r\n"
            + archive.getvalue()
            + b"\r\n--" + boundary.encode() + b"--\r\n"
        )
        status, result, _ = self.call(
            "/api/plugins/upload", method="POST", raw=body,
            headers={"Content-Type": "multipart/form-data; boundary=" + boundary},
            timeout=30,
        )
        check(status == 200 and "mediamesh" in result.get("installed", []),
              "install MediaMesh through the live Silo plugin manager")
        session = wait_for(
            "MediaMesh plugin routes",
            lambda: self._plugin_session(),
            20,
        )
        status, html, _ = self.call("/plugins/mediamesh/")
        check(status == 200 and b"MediaMesh" in html,
              "Silo serves the installed MediaMesh page")
        return session

    def _plugin_session(self):
        status, data, _ = self.call("/plugins/mediamesh/api/session")
        return data if status == 200 and isinstance(data, dict) and data.get("csrf") else None

    def configure_plugin(self, session, daemon_url):
        status, result, _ = self.call(
            "/plugins/mediamesh/api/settings", method="POST",
            body={"url": daemon_url, "token": P2P_TOKEN},
            headers={"X-MediaMesh-CSRF": session["csrf"]},
        )
        check(status == 200 and result.get("token_configured") is True,
              "configure MediaMesh with the subscriber daemon and server-side token")


def prepare_torrent(root, tracker_url, seed_port):
    folder = Path(root) / "seed" / "MediaMesh"
    folder.mkdir(parents=True)
    data = bytes((i * 73 + 19) % 256 for i in range(256 * 1024))
    (folder / "playback-fixture.bin").write_bytes(data)
    storage = lt.file_storage()
    lt.add_files(storage, str(folder))
    creator = lt.create_torrent(storage, 16 * 1024)
    creator.add_tracker(tracker_url)
    lt.set_piece_hashes(creator, str(folder.parent))
    info = lt.torrent_info(creator.generate())
    seeder = Seeder(info, folder.parent, seed_port)
    return info, data, seeder


def start_p2p_mesh(daemon, root, used_ports):
    address = local_ip()
    ports = {
        name: {
            "http": free_port(used_ports),
            "peer": free_port(used_ports),
        }
        for name in P2P_NODES
    }
    nodes = {}
    for name in P2P_NODES:
        bootstrap = ",".join(
            f"{address}:{ports[other]['peer']}"
            for other in P2P_NODES if other != name
        )
        node = P2PNode(
            daemon, root, name, ports[name]["http"], ports[name]["peer"], bootstrap
        )
        nodes[name] = node
        node.start()
    return nodes, address


def make_silo_checkout(upstream_arg, work):
    tree = Path(work) / "torrserver"
    if upstream_arg:
        source = Path(upstream_arg).resolve()
        head = subprocess.check_output(
            ["git", "-C", str(source), "rev-parse", "HEAD"], text=True
        ).strip()
        dirty = subprocess.check_output(
            ["git", "-C", str(source), "status", "--porcelain"], text=True
        ).strip()
        if head != PIN or dirty:
            raise RuntimeError(f"--upstream must be clean at {PIN}, got {head}; dirty={bool(dirty)}")
        shutil.copytree(source, tree, ignore=shutil.ignore_patterns(".git"))
    else:
        subprocess.run(
            ["git", "clone", "--quiet", "--branch", "silo", UPSTREAM, str(tree)],
            check=True,
        )
        subprocess.run(
            ["git", "-C", str(tree), "checkout", "--quiet", "--detach", PIN],
            check=True,
        )
    return tree


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("daemon", help="built P2Play p2playd executable")
    parser.add_argument("--upstream", help="clean Silo checkout at the pinned commit")
    parser.add_argument("--go", default=shutil.which("go") or "go", help="Go executable")
    args = parser.parse_args()

    daemon = str(Path(args.daemon).resolve())
    if not Path(daemon).is_file():
        parser.error("p2playd executable does not exist")
    if not shutil.which(args.go):
        parser.error("Go 1.26+ is required")
    if not PLUGIN_DIR.is_dir():
        parser.error("plugins/mediamesh directory not found")

    work = tempfile.mkdtemp(prefix="p2play-silo-live-")
    p2p_nodes = {}
    silo = None
    tracker = None
    seeder = None
    success = False
    used_ports = set()
    try:
        upstream = make_silo_checkout(args.upstream, work)
        test_file = upstream / "internal" / "plugin" / "mediamesh_test.go"
        shutil.copyfile(HERE / "tools" / "silo-plugin-test.go", test_file)
        env = dict(os.environ)
        env["MEDIAMESH_PLUGIN_DIR"] = str(PLUGIN_DIR)
        subprocess.run(
            [args.go, "test", "./internal/plugin", "-run", "^TestMediaMesh",
             "-count=1", "-v"],
            cwd=upstream, env=env, check=True,
        )
        silo_binary = Path(work) / "silo-server"
        subprocess.run(
            [args.go, "build", "-trimpath", "-o", str(silo_binary), "./cmd/server"],
            cwd=upstream, check=True,
        )
        check(True, "pinned Silo host-contract tests and real server build")

        silo_port = free_port(used_ports)
        silo = SiloServer(str(silo_binary), work, silo_port)
        silo.start()
        silo.login()
        silo.set_isolated_torrent_config()

        # Build a single-file v1 torrent and seed it only on loopback. The Silo
        # engine learns the seed address from this test's private HTTP tracker.
        seed_port = free_port(used_ports)
        tracker = LocalTracker(seed_port)
        info, media_bytes, seeder = prepare_torrent(work, tracker.url, seed_port)
        infohash = str(info.info_hashes().v1)
        check(len(infohash) == 40, "create a real v1 media torrent")

        p2p_nodes, _mesh_address = start_p2p_mesh(daemon, Path(work) / "p2p", used_ports)
        publisher = p2p_nodes["publisher"]
        subscriber = p2p_nodes["silo-node"]
        second = p2p_nodes["second-holder"]

        status, identity, _ = publisher.call(
            "/api/publish", method="POST",
            body={"name": "Silo live integration", "description": "native playback test"},
        )
        pubkey = identity.get("pubkey", "") if isinstance(identity, dict) else ""
        check(status == 201 and len(pubkey) == 64, "publish MediaMesh catalog identity")
        record = {
            "ih": infohash,
            "title": "Silo live playback fixture",
            "size": len(media_bytes),
            "files": 1,
            "tags": ["movie", "integration"],
            "trackers": [tracker.url],
        }
        status, result, _ = publisher.call(
            f"/api/publish/{pubkey}/records", method="POST",
            raw=(json.dumps(record, separators=(",", ":")) + "\n").encode(),
        )
        check(status == 200 and result.get("count") == 1,
              "publish the signed catalog record with a loopback tracker")
        status, release, _ = publisher.call(
            f"/api/publish/{pubkey}/release", method="POST", body={},
        )
        check(status == 200 and release.get("seq") == 1,
              "release signed catalog version 1")
        wait_for(
            "publisher version torrent seed",
            lambda: (publisher.call("/api/lists")[1]["lists"][0]
                     .get("net", {}).get("seeding")),
            30,
        )

        status, _, _ = second.call(f"/api/lists/{pubkey}", method="POST", body={})
        check(status == 202, "second node subscribes as a directory holder")
        wait_for(
            "second node receives the signed catalog",
            lambda: next((item for item in second.call("/api/lists")[1]["lists"]
                          if item.get("pubkey") == pubkey
                          and item.get("net", {}).get("records_ready")), None),
            90,
        )
        wait_for(
            "Silo-connected P2Play node discovers the catalog",
            lambda: next((item for item in subscriber.call("/api/lists/discover")[1]["lists"]
                          if item.get("pubkey") == pubkey), None),
            90,
        )

        session = silo.upload_plugin()
        silo.configure_plugin(session, subscriber.base)
        status, discovered, _ = silo.call("/plugins/mediamesh/api/discover")
        check(status == 200 and any(
            item.get("pubkey") == pubkey for item in discovered.get("lists", [])
        ), "Silo plugin discovers the publisher's list through P2Play")
        status, subscribed, _ = silo.call(
            "/plugins/mediamesh/api/subscribe", method="POST",
            body={"pubkey": pubkey},
            headers={"X-MediaMesh-CSRF": session["csrf"]},
        )
        check(status == 202 and subscribed.get("status") == "subscribing",
              "Silo plugin subscribes through the native P2Play daemon")

        wait_for(
            "P2Play subscriber verifies and indexes the list",
            lambda: next((item for item in subscriber.call("/api/lists")[1]["lists"]
                          if item.get("pubkey") == pubkey
                          and item.get("net", {}).get("records_ready")), None),
            90,
        )
        status, lists, _ = silo.call("/plugins/mediamesh/api/lists")
        check(status == 200 and any(
            item.get("pubkey") == pubkey for item in lists.get("lists", [])
        ), "Silo plugin shows the subscribed catalog")
        query = urllib.parse.urlencode({"q": "live playback"})
        status, search, _ = silo.call("/plugins/mediamesh/api/search?" + query)
        results = search.get("results", []) if isinstance(search, dict) else []
        check(status == 200 and any(
            item.get("hash") == infohash and item.get("title") == "Silo live playback fixture"
            for item in results
        ), "Silo plugin searches the verified MediaMesh record")

        status, added, _ = silo.call(
            "/plugins/mediamesh/api/add", method="POST",
            body={"hash": infohash},
            headers={"X-MediaMesh-CSRF": session["csrf"]},
        )
        check(status == 201 and added.get("hash") == infohash,
              "plugin invokes the real native Silo torrent.add binding")

        def silo_metadata():
            status, status_data, _ = silo.call(f"/api/torrents/{infohash}")
            if status == 200 and status_data.get("file_stats"):
                return status_data
            return None

        torrent_status = wait_for("Silo native torrent metadata", silo_metadata, 60)
        status, library, _ = silo.call("/api/torrents")
        check(status == 200 and any(
            item.get("hash") == infohash for item in library.get("torrents", [])
        ), "torrent is stored in the authenticated Silo user's library")
        file_index = torrent_status["file_stats"][0]["id"]
        status, streamed, headers = silo.call(
            f"/api/stream/{infohash}/{file_index}",
            headers={"Range": "bytes=0-4095"},
            timeout=45,
        )
        check(status == 206 and streamed == media_bytes[:4096]
              and headers.get("Content-Range", "").startswith("bytes 0-4095/"),
              "Silo native stream serves verified bytes from the local BitTorrent seeder")
        check(tracker.announces > 0, "Silo native engine used the local HTTP tracker")

        # num_peers is only a point-in-time snapshot and commonly returns to
        # zero immediately after the requested range has been delivered. Check
        # the seeder's cumulative payload counter instead: it proves real
        # BitTorrent payload crossed the peer connection even after disconnect.
        def uploaded_payload():
            count = seeder.handle.status().total_payload_upload
            return count if count >= 4096 else None

        uploaded = wait_for("Silo received payload from the loopback seeder",
                            uploaded_payload, 10)
        check(uploaded >= 4096,
              f"Silo native engine downloaded payload from the loopback seeder ({uploaded} bytes)")
        print("PASS live Silo + P2Play MediaMesh discovery, subscription, search, add and playback",
              flush=True)
        success = True
    except Exception:
        if silo:
            print("---- Silo log ----", flush=True)
            print(silo.read_log()[-12000:], flush=True)
        for node in p2p_nodes.values():
            print(f"---- P2Play {node.name} log ----", flush=True)
            print(node.read_log()[-7000:], flush=True)
        raise
    finally:
        for node in p2p_nodes.values():
            node.stop()
        if silo:
            silo.stop()
        if seeder:
            seeder.close()
        if tracker:
            tracker.close()
        if success and not os.environ.get("KEEP"):
            shutil.rmtree(work, ignore_errors=True)
        else:
            print("Live Silo test files retained at: " + work, flush=True)


if __name__ == "__main__":
    main()
