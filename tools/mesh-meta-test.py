#!/usr/bin/env python3
"""Signed MediaMesh meta-list manifest/API, import and depth-bound tests.

    python3 -I tools/mesh-meta-test.py build/src/app/p2playd
"""
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

TOKEN = "ts_meta00000000000000000000"


def check(ok, message):
    print(("OK   " if ok else "FAIL ") + message, flush=True)
    if not ok:
        raise AssertionError(message)


class Node:
    def __init__(self, daemon, data):
        self.daemon = daemon
        self.data = Path(data)
        self.data.mkdir(parents=True, exist_ok=True)
        self.proc = None
        self.log = None
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            self.port = sock.getsockname()[1]
        (self.data / "auth.json").write_text(json.dumps({
            "users": [{"username": "owner", "password": "test", "api_token": TOKEN}]
        }))
        (self.data / "settings.json").write_text(json.dumps({"DisableDHT": True}))

    def call(self, path, body=None, method=None):
        payload = None if body is None else json.dumps(body, ensure_ascii=False).encode()
        req = urllib.request.Request(
            f"http://127.0.0.1:{self.port}{path}", data=payload,
            method=method or ("POST" if payload is not None else "GET"))
        req.add_header("Authorization", "Bearer " + TOKEN)
        if payload is not None:
            req.add_header("Content-Type", "application/json")
        try:
            response = urllib.request.urlopen(req, timeout=30)
        except urllib.error.HTTPError as exc:
            response = exc
        with response:
            raw = response.read()
            try:
                data = json.loads(raw) if raw else {}
            except ValueError:
                data = raw.decode("utf-8", "replace")
            return response.status, data

    def start(self):
        self.log = (self.data / "stdout.log").open("a")
        self.proc = subprocess.Popen(
            [self.daemon, "--data", str(self.data), "--port", str(self.port)],
            stdout=self.log, stderr=subprocess.STDOUT, cwd=self.data)
        for _ in range(200):
            if self.proc.poll() is not None:
                raise RuntimeError("daemon exited: " + (self.data / "stdout.log").read_text()[-4000:])
            try:
                if self.call("/echo")[0] == 200:
                    return
            except (OSError, urllib.error.URLError):
                pass
            time.sleep(0.05)
        raise RuntimeError("daemon did not start")

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


def make_list(node, name, kind="catalog"):
    status, result = node.call("/api/publish", {
        "name": name, "description": "meta-list test fixture", "type": kind
    })
    key = result.get("pubkey", "")
    check(status == 201 and len(key) == 64, f"create {kind} list: {name}")
    return key


def main():
    daemon = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/src/app/p2playd")
    root = Path(tempfile.mkdtemp(prefix="p2play-meta-list-"))
    publisher = Node(daemon, root / "publisher")
    subscriber = Node(daemon, root / "subscriber")
    try:
        publisher.start()
        enabled = make_list(publisher, "Enabled leaf")
        disabled = make_list(publisher, "Disabled leaf")
        nested = make_list(publisher, "Nested meta", "meta")
        collection = make_list(publisher, "Curated collection", "meta")

        endpoint = f"/api/publish/{collection}/meta"
        status, result = publisher.call(endpoint, {"items": [
            {"pubkey": collection, "annotation": "self", "enabled": True}
        ]})
        check(status == 400, "self-reference refused")
        status, result = publisher.call(endpoint, {"items": [
            {"pubkey": "f" * 64, "annotation": "missing", "enabled": True}
        ]})
        check(status == 400, "unknown child key refused")
        status, result = publisher.call(endpoint, {"items": [
            {"pubkey": nested, "annotation": "nested", "enabled": True}
        ]})
        check(status == 400, "nested meta-list refused by publisher")

        status, result = publisher.call(endpoint, {"items": [
            {"pubkey": enabled, "annotation": "Included family films", "enabled": True},
            {"pubkey": disabled, "annotation": "Temporarily hidden", "enabled": False}
        ]})
        check(status == 200 and result.get("items") == 2, "two signed meta items staged")
        status, state = publisher.call(f"/api/publish/{collection}/status")
        check(status == 200 and state.get("type") == "meta" and state.get("pending_meta") is True,
              "meta-list type and pending state visible")
        status, released = publisher.call(f"/api/publish/{collection}/release", {})
        check(status == 200 and released.get("seq") == 1 and released.get("items", [])[0]["enabled"],
              "metadata-only release signed at sequence 1")

        status, result = publisher.call(endpoint, {"items": [
            {"pubkey": enabled, "annotation": "Included family films", "enabled": False},
            {"pubkey": disabled, "annotation": "Curated documentaries", "enabled": True}
        ]})
        check(status == 200, "meta-list item enable flags can be changed")
        status, released = publisher.call(f"/api/publish/{collection}/release", {})
        check(status == 200 and released.get("seq") == 2, "meta-list edit released as sequence 2")
        current = next(x for x in released["items"] if x["pubkey"] == disabled)
        check(current["enabled"] is True and current["annotation"] == "Curated documentaries",
              "updated annotation and enabled state appear in signed result")

        status, result = publisher.call(endpoint, {"items": [
            {"pubkey": enabled, "annotation": "Included family films", "enabled": True},
            {"pubkey": disabled, "annotation": "Temporarily hidden", "enabled": False}
        ]})
        check(status == 200, "meta-list selection can be restored")
        status, released = publisher.call(f"/api/publish/{collection}/release", {})
        check(status == 200 and released.get("seq") == 3, "restored selection released as sequence 3")

        # Import the enabled leaf and collection on a second node. The disabled leaf is
        # intentionally absent: syncing the collection must not subscribe to that entry.
        sources = root / "sources"
        sources.mkdir()
        for key in (enabled, disabled, nested, collection):
            shutil.copytree(publisher.data / "lists" / key, sources / key)

        bad = root / "tampered-meta"
        shutil.copytree(sources / collection, bad)
        manifest_path = bad / "manifest.ben"
        manifest = manifest_path.read_bytes()
        tampered = manifest.replace(b"Curated documentaries", b"Updated documentaries", 1)
        check(tampered != manifest, "meta annotation present in signed manifest fixture")
        manifest_path.write_bytes(tampered)

        subscriber.start()
        status, result = subscriber.call("/api/lists/import", {"dir": str(sources / enabled)})
        check(status == 200, "ordinary child list imported")
        status, result = subscriber.call("/api/lists/import", {"dir": str(sources / nested)})
        check(status == 200, "standalone meta-list can be imported as a root")
        status, result = subscriber.call("/api/lists/import", {"dir": str(bad)})
        check(status == 400 and "signature" in result.get("error", "").lower(),
              "tampered meta annotation rejected by manifest signature")
        status, result = subscriber.call("/api/lists/import", {"dir": str(sources / collection)})
        check(status == 200, "signed meta-list imported")
        lists = subscriber.call("/api/lists")[1].get("lists", [])
        by_key = {item["pubkey"]: item for item in lists}
        check(collection in by_key and by_key[collection]["type"] == "meta",
              "subscriber exposes meta-list type")
        items = by_key[collection].get("items", [])
        check(len(items) == 2 and items[0]["annotation"] == "Included family films" and
              items[0]["enabled"] is False and items[1]["enabled"] is True,
              "subscriber receives ordered annotations and enable flags")
        check(enabled in by_key and disabled not in by_key,
              "only enabled child is followed; disabled child is not subscribed")
        check(nested in by_key and by_key[nested]["type"] == "meta",
              "a meta-list may exist independently as a root")

        subscriber.stop()
        subscriber.start()
        lists = subscriber.call("/api/lists")[1].get("lists", [])
        by_key = {item["pubkey"]: item for item in lists}
        check(collection in by_key and by_key[collection]["items"][1]["enabled"],
              "meta-list and child policy survive daemon restart")
        print("PASS")
    finally:
        publisher.stop()
        subscriber.stop()
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
