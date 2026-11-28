#!/usr/bin/env python3
"""Real Mesh storage + native HTTP media/shard tests, using only the Python stdlib.

  python3 -I tools/mesh-media-test.py --build-dir build-linux build-linux/src/app/p2playd
  python3 -I tools/mesh-media-test.py build-linux/src/app/p2playd  # HTTP only
  python3 -I tools/mesh-media-test.py --build-dir build-linux --sanitize  # storage only
  python3 -I tools/mesh-media-test.py  # storage with system pkg-config libtorrent

--build-dir uses the native Unix Makefiles CMake build's actual compiler definitions,
headers and pinned libtorrent archive. No mocked crypto, fake pack code or Python bindings.
CXX/CXXFLAGS/LDFLAGS can instead select a system libtorrent development installation.
"""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import shlex
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

REPO = Path(__file__).resolve().parent.parent
TOKEN = "ts_media000000000000000000"
CHECKS = 0


def check(ok, message):
    global CHECKS
    if not ok:
        raise AssertionError(message)
    CHECKS += 1
    print("OK   " + message, flush=True)


def unit(args, root):
    cxx = shlex.split(os.environ.get("CXX", "c++"))
    flags = ["-std=c++17", "-O1", "-g", "-Wall", "-Wextra"]
    if args.build_dir:
        build = Path(args.build_dir).resolve()
        make_flags = build / "src/core/CMakeFiles/p2play_core.dir/flags.make"
        if not make_flags.is_file():
            raise RuntimeError("--build-dir requires a native Unix Makefiles CMake build (or use CXXFLAGS/LDFLAGS)")
        variables = dict(line.split(" = ", 1) for line in make_flags.read_text().splitlines() if " = " in line)
        flags += shlex.split(variables["CXX_DEFINES"]) + shlex.split(variables["CXX_INCLUDES"])
        libraries = [str(build / "third_party/libtorrent/libtorrent-rasterbar.a"), "-pthread"]
    else:
        flags += shlex.split(subprocess.check_output(["pkg-config", "--cflags", "libtorrent-rasterbar"], text=True))
        libraries = shlex.split(subprocess.check_output(["pkg-config", "--libs", "libtorrent-rasterbar"], text=True)) + ["-pthread"]
    flags += shlex.split(os.environ.get("CXXFLAGS", ""))
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    output = root / "mesh-media-unit"
    command = cxx + flags + [str(REPO / "tools/mesh-media-test.cpp"), str(REPO / "src/core/mesh.cpp"),
                              str(REPO / "src/core/torrs.cpp")] + libraries + shlex.split(os.environ.get("LDFLAGS", "")) + ["-o", str(output)]
    subprocess.run(command, check=True, cwd=root)
    scratch = root / "unit-data"
    scratch.mkdir()
    subprocess.run([str(output), str(scratch)], check=True, cwd=root)


class Node:
    def __init__(self, daemon, data):
        self.daemon, self.data, self.proc, self.log = daemon, data, None, None
        self.data.mkdir(parents=True, exist_ok=True)
        (self.data / "auth.json").write_text(json.dumps({"users": [{"username": "owner", "password": "test", "api_token": TOKEN}]}))
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            self.port = s.getsockname()[1]

    def start(self):
        self.log = (self.data / "stdout.log").open("a")
        self.proc = subprocess.Popen([self.daemon, "--data", str(self.data), "--port", str(self.port)],
                                     stdout=self.log, stderr=subprocess.STDOUT, cwd=self.data)
        for _ in range(200):
            if self.proc.poll() is not None:
                raise RuntimeError("daemon exited: " + (self.data / "stdout.log").read_text()[-4000:])
            try:
                if self.call("/api/v1/system/ping")[0] == 200:
                    return
            except (OSError, urllib.error.URLError):
                pass
            time.sleep(0.05)
        raise RuntimeError("daemon not ready")

    def stop(self):
        if self.proc:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
            self.proc = None
        if self.log:
            self.log.close()
            self.log = None

    def call(self, path, body=None, raw=None, method=None, content_type=None, token=TOKEN):
        payload = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
        req = urllib.request.Request(f"http://127.0.0.1:{self.port}" + path, data=payload,
                                     method=method or ("POST" if payload is not None else "GET"))
        if token:
            req.add_header("Authorization", "Bearer " + token)
        if content_type:
            req.add_header("Content-Type", content_type)
        elif body is not None:
            req.add_header("Content-Type", "application/json")
        try:
            response = urllib.request.urlopen(req, timeout=30)
        except urllib.error.HTTPError as exc:
            response = exc
        with response:
            data = response.read()
            if response.headers.get("Content-Type", "").startswith("application/json") and data:
                data = json.loads(data)
            return response.status, data, response.headers


def http_tests(daemon, root):
    a, b = Node(daemon, root / "http-a"), Node(daemon, root / "http-b")
    try:
        a.start()
        status, result, _ = a.call("/api/v1/publish", {"name": "Media HTTP", "description": "Integration test"})
        pk = result.get("pubkey", "")
        check(status == 201 and len(pk) == 64, "HTTP publisher identity")
        publish = f"/api/v1/publish/{pk}"
        desc = "Description, ёлки.".encode()
        png = b"\x89PNG\r\n\x1a\n" + b"poster" * 32
        status, dref, _ = a.call(publish + "/media/description", raw=desc, content_type="text/plain; charset=utf-8")
        check(status == 201 and dref["length"] == len(desc), "HTTP raw description upload returns slice reference")
        status, pref, _ = a.call(publish + "/media/poster", raw=png, content_type="image/png")
        check(status == 201 and pref["sha256"], "HTTP poster upload")
        check(a.call(publish + "/media/poster", raw=b"<svg/>", content_type="image/svg+xml")[0] == 400, "HTTP active MIME refused")
        check(a.call(publish + "/media/description", raw=desc, token="")[0] in (401, 403), "HTTP media publishing requires authorization")
        ih, other = "a" * 40, "b" * 40
        record = {"ih": ih, "title": "Alpha", "shard": "movies", "description_ref": dref, "poster_ref": pref}
        records = json.dumps(record) + "\n" + json.dumps({"ih": other, "title": "Beta", "shard": "series"}) + "\n"
        check(a.call(publish + "/records", raw=records.encode())[0] == 200 and a.call(publish + "/release", {})[0] == 200,
              "HTTP release signed media/shard version")
        status, data, headers = a.call(f"/api/v1/catalog/{ih}/description?list={pk}")
        check(status == 200 and data == desc and headers.get("X-Content-Type-Options") == "nosniff", "HTTP lazy description and nosniff")
        check(a.call(f"/api/v1/catalog/{ih}/poster")[1] == png, "HTTP binary poster response")
        status, data, headers = a.call(f"/api/v1/catalog/{ih}/poster", method="HEAD")
        check(status == 200 and data == b"" and int(headers["Content-Length"]) == len(png), "HTTP media HEAD without response body")
        check(a.call(f"/api/v1/catalog/{ih}/poster?list=" + "f" * 64)[0] == 404, "HTTP list-specific media access checks membership")
        b.start()
        src = a.data / "lists" / pk
        check(b.call("/api/v1/lists/import", {"dir": str(src)})[0] == 200, "HTTP import verifies descriptors and payloads")
        policy = f"/api/v1/lists/{pk}/policy"
        check(b.call(policy, {"shards": ["movies"], "media_bytes": 0, "cache_bytes": 0})[0] == 200, "HTTP persisted shard/media/cache policy")
        result = b.call("/api/v1/catalog/search")[1]["results"]
        check([r["hash"] for r in result] == [ih], "HTTP only selected shard indexed")
        check(b.call(f"/api/v1/catalog/{ih}/poster")[0] == 503, "HTTP unavailable media is explicit, no remote URL fetching")
        check(b.call(policy, {"cache_bytes": 2 ** 64 - 1})[0] == 400 and b.call(policy, {"shards": ["../escape"]})[0] == 400,
              "HTTP overflowing budgets and unsafe shard names refused")
        check(b.call(policy, {"media_bytes": 16 << 20, "cache_bytes": 1024})[0] == 200, "HTTP raise lazy media budget")
        check(b.call(f"/api/v1/lists/{pk}/files", {"path": "media/" + pref["pack"], "source": str(src / "media" / pref["pack"])})[0] == 200,
              "HTTP authenticated bounded media install")
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(lambda _: b.call(f"/api/v1/catalog/{ih}/poster"), range(20)))
        check(all(r[0] == 200 and r[1] == png for r in results), "concurrent media cache requests serialized safely")
        b.stop()
        b.start()
        check(b.call(policy)[1]["policy"]["shards"] == ["movies"] and b.call(f"/api/v1/catalog/{ih}/poster")[1] == png,
              "HTTP policy, cached media and record index persist after restart")
        status, snapshot, _ = a.call(publish + "/snapshot", {})
        check(status == 200 and snapshot["seq"] == 2 and a.call(f"/api/v1/catalog/{ih}/description")[1] == desc,
              "HTTP snapshot atomically repacks live media and records")
        check(b.call(f"/api/v1/lists/{pk}", method="DELETE")[0] == 200 and not (b.data / "mesh-local" / pk).exists(),
              "HTTP removal drops received shards, packs and local cache")
        print("PASS HTTP", CHECKS, "checks", flush=True)
    finally:
        a.stop()
        b.stop()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("daemon", nargs="?", help="p2playd executable: run native API integration checks")
    parser.add_argument("--build-dir", help="native CMake Unix Makefiles build: also compile and run storage integration tests")
    parser.add_argument("--sanitize", action="store_true", help="ASan/UBSan for the storage harness and production Mesh/Torrs code")
    args = parser.parse_args()
    if args.daemon:
        args.daemon = str(Path(args.daemon).resolve())
    with tempfile.TemporaryDirectory(prefix="p2play-mesh-media-") as scratch:
        root = Path(scratch)
        if args.build_dir or not args.daemon:
            unit(args, root)
        if args.daemon:
            http_tests(args.daemon, root)


if __name__ == "__main__":
    main()
