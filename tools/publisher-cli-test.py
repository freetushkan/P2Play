#!/usr/bin/env python3
"""Publisher CLI tests against a local HTTP fixture; no daemon or third-party packages."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CLI = Path(__file__).resolve().parent / "p2play"
PUBKEY = "a" * 64


class FixtureServer(ThreadingHTTPServer):
    def __init__(self, address, handler):
        super().__init__(address, handler)
        self.seen = []
        self.records = None
        self.created = False
        self.released = False


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def _send(self, status, value):
        raw = json.dumps(value).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def _body(self):
        return self.rfile.read(int(self.headers.get("Content-Length", "0")))

    def do_GET(self):
        self.server.seen.append(("GET", self.path, self.headers.get("Authorization")))
        if self.path == "/api/lists":
            self._send(200, {"lists": [
                {"pubkey": PUBKEY, "own": True, "type": "catalog"},
                {"pubkey": "b" * 64, "own": False, "type": "catalog"},
            ]})
        elif self.path == f"/api/publish/{PUBKEY}/status":
            self._send(200, {"pubkey": PUBKEY, "pending": len(self.server.records or [])})
        else:
            self._send(404, {"error": "not found"})

    def do_POST(self):
        body = self._body()
        self.server.seen.append(("POST", self.path, self.headers.get("Authorization")))
        if self.path == "/api/auth/login":
            data = json.loads(body)
            if data != {"username": "owner", "password": "test-secret"}:
                self._send(401, {"error": "bad credentials"})
            else:
                self._send(200, {"token": "fixture-token"})
        elif self.path == "/api/publish":
            self.server.created = True
            self._send(201, {"pubkey": PUBKEY})
        elif self.path == f"/api/publish/{PUBKEY}/media/description":
            self._send(201, {
                "pack": "p-" + "c" * 64 + ".bin", "sha256": "d" * 64,
                "offset": 0, "length": len(body), "type": "text/plain; charset=utf-8",
            })
        elif self.path == f"/api/publish/{PUBKEY}/media/poster":
            self._send(201, {
                "pack": "p-" + "c" * 64 + ".bin", "sha256": "d" * 64,
                "offset": 0, "length": len(body), "type": "image/png",
            })
        elif self.path == f"/api/publish/{PUBKEY}/records":
            self.server.records = [json.loads(line) for line in body.decode().splitlines() if line]
            self._send(200, {"count": len(self.server.records), "pending": len(self.server.records)})
        elif self.path == f"/api/publish/{PUBKEY}/release":
            self.server.released = True
            self._send(200, {"pubkey": PUBKEY, "seq": 1, "records": len(self.server.records or [])})
        else:
            self._send(404, {"error": "not found"})


def check(value, message):
    if not value:
        raise AssertionError(message)
    print("OK   " + message)


def run_cli(args):
    env = os.environ.copy()
    env["P2PLAY_PASSWORD"] = "test-secret"
    return subprocess.run(
        [sys.executable, str(CLI), *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, env=env, timeout=20
    )


def main():
    server = FixtureServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}"
    try:
        with tempfile.TemporaryDirectory(prefix="p2play-publisher-") as tmp:
            root = Path(tmp)
            (root / "poster.png").write_bytes(b"\x89PNG\r\n\x1a\nfixture-png")
            category_map = root / "categories.json"
            category_map.write_text(json.dumps({"movie": ["film", "video"]}), encoding="utf-8")
            ndjson = root / "records.ndjson"
            records = [
                {"ih": "A" * 40, "name": "Тестовый фильм", "category": "Movie",
                 "date": "2024-01-02T03:04:05Z", "description": "Краткое описание",
                 "poster_file": "poster.png", "site": "example.invalid", "src": "fixture"},
                {"del": "B" * 40, "shard": "default"},
                {"ih": "A" * 40, "name": "Тестовый фильм", "category": "Movie",
                 "date": "2024-01-02T03:04:05Z", "description": "Краткое описание",
                 "poster_file": "poster.png", "site": "example.invalid", "src": "fixture"},
            ]
            ndjson.write_text("\n".join(json.dumps(x, ensure_ascii=False) for x in records) + "\n",
                              encoding="utf-8")
            dry = run_cli(["publish", "--import", str(ndjson), "--dry-run",
                           "--category-map", str(category_map), "--server", base])
            check(dry.returncode == 0 and "Dry-run: 2 records, 1 identical duplicates skipped" in dry.stdout,
                  "dry-run normalizes rows and skips identical duplicate infohashes")
            check(not server.seen, "dry-run performs no HTTP requests")

            result = run_cli(["publish", "--import", str(ndjson), "--server", base,
                              "--category-map", str(category_map),
                              "--create-list", "Fixture catalog", "--release"])
            check(result.returncode == 0, "publish command succeeds: " + result.stderr)
            check(server.created, "a new list is created through the P2Play API")
            check(server.records is not None and len(server.records) == 2, "only unique rows are staged")
            item = next(x for x in server.records if "ih" in x)
            check(item["ih"] == "a" * 40 and item["title"] == "Тестовый фильм",
                  "name and uppercase infohash normalize to the MediaMesh record schema")
            check(item["date"] == 1704164645, "ISO-8601 date converts to Unix seconds")
            check(item["tags"] == ["film", "video"], "category mapping produces tags")
            check(item["description_ref"]["type"] == "text/plain; charset=utf-8",
                  "description content uploads and is replaced by a signed media reference")
            check(item["poster_ref"]["type"] == "image/png",
                  "poster file is MIME-checked and uploaded")
            check(server.released, "--release signs the next version")
            check(all(auth == "Bearer fixture-token" for method, path, auth in server.seen
                      if path != "/api/auth/login"), "protected requests use the session token")
            check("fixture-token" not in result.stdout + result.stderr, "API token is never printed")

            unowned = run_cli(["publish", "--import", str(ndjson), "--server", base, "--list", "b" * 64])
            check(unowned.returncode == 2 and "not owned" in unowned.stderr,
                  "publisher refuses a list not owned by this server")
            bad = root / "bad.ndjson"
            bad.write_text(json.dumps({"ih": "a" * 40, "title": "bad",
                                       "unknown_private_field": "x"}) + "\n", encoding="utf-8")
            rejected = run_cli(["publish", "--import", str(bad), "--dry-run"])
            check(rejected.returncode == 2 and "unknown fields" in rejected.stderr,
                  "unknown fields are rejected before network mutation")

            for field, value in (("files", 1 << 31), ("size", 1 << 63), ("date", 1 << 63)):
                invalid = root / (field + "-overflow.ndjson")
                invalid.write_text(json.dumps({"ih": "c" * 40, "title": "range check",
                                               field: value}) + "\n", encoding="utf-8")
                failed = run_cli(["publish", "--import", str(invalid), "--dry-run"])
                check(failed.returncode == 2 and field in failed.stderr,
                      field + " overflow is rejected against MediaMesh API integer limits")

            remote_http = run_cli(["publish", "--import", str(ndjson), "--server",
                                   "http://publisher.example.invalid", "--list", PUBKEY])
            check(remote_http.returncode == 2 and "plain HTTP" in remote_http.stderr,
                  "remote plain HTTP is refused before credentials are sent")
            check(not any(path == "/api/auth/login" for method, path, auth in server.seen),
                  "refused transport does not attempt authentication")
        print("PASS")
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


if __name__ == "__main__":
    main()
