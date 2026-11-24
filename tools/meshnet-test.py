#!/usr/bin/env python3
"""MediaMesh over the network: two nodes on this machine with a DHT of their own.

    tools/meshnet-test.py build-linux/src/app/p2playd

Checks (a third node, C, only stores DHT items): node A publishes a list (its version seeded as a v2 torrent, its pointer put into the
DHT); node B subscribes by the key alone: the pointer from the DHT, the version downloaded
from A, checked and searched; a newer version reaches B; B seeds what it took; C, holding no
list, learns of it and its holders in the directory swarm ("mediamesh" extension).
"""

import base64
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

spec = importlib.util.spec_from_file_location("core_test", os.path.join(os.path.dirname(__file__), "core-test.py"))
ct = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ct)
check = ct.check

# a, b and a third node c: an item put by a node is stored by the others, not by itself
NODES = {"a": (8193, 6931), "b": (8194, 6932), "c": (8195, 6933)}


def local_ip():
    """the address of the way out (the DHT keeps no loopback nodes)"""
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 53))
        return s.getsockname()[0]
    finally:
        s.close()


def call(node, path, body=None, method=None, raw=None):
    data = raw if raw is not None else (None if body is None else json.dumps(body).encode())
    req = urllib.request.Request(f"http://127.0.0.1:{NODES[node][0]}" + path, data=data,
                                 method=method or ("POST" if data is not None else "GET"))
    req.add_header("Authorization", "Bearer " + ct.OWNER_TOKEN)
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def start(daemon, root, node):
    http, bt = NODES[node]
    others = ",".join(f"{local_ip()}:{bt}" for n, (_, bt) in NODES.items() if n != node)
    data = os.path.join(root, node)
    os.makedirs(data, exist_ok=True)
    with open(os.path.join(data, "settings.json"), "w") as f:
        json.dump({"PeersListenPort": bt, "P2PDhtBootstrap": others, "P2PCatalogOnly": node == "c"}, f)
    with open(os.path.join(data, "auth.json"), "w") as f:
        json.dump({"users": [{"username": "owner", "password": ct.OWNER_PASSWORD, "api_token": ct.OWNER_TOKEN}]}, f)
    log = open(os.path.join(data, "stdout.log"), "a")
    proc = subprocess.Popen([daemon, "--data", data, "--port", str(http)], stdout=log, stderr=subprocess.STDOUT)
    for _ in range(100):
        try:
            if call(node, "/api/system/ping")[0] == 200:
                return proc
        except Exception:
            pass
        time.sleep(0.1)
    raise RuntimeError("p2playd did not start")


def wait(what, cond, seconds=90):
    end = time.time() + seconds
    while time.time() < end:
        v = cond()
        if v:
            return v
        time.sleep(1)
    return None


def main():
    daemon = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build-linux/src/app/p2playd")
    root = tempfile.mkdtemp(prefix="p2play-meshnet-")
    procs = []
    try:
        procs.append(start(daemon, root, "a"))
        procs.append(start(daemon, root, "b"))
        procs.append(start(daemon, root, "c"))
        pk = call("a", "/api/publish", {"name": "Net list"})[1]["pubkey"]
        call("a", f"/api/publish/{pk}/records",
             raw=json.dumps({"ih": "d" * 40, "title": "Network Movie 2026", "tags": ["movie"]}).encode())
        status, out = call("a", f"/api/publish/{pk}/release", {})
        check(status == 200 and out["net"]["seeding"] and len(out["net"].get("version_ih", "")) == 64,
              "A: version 1 seeded as a v2 torrent")
        time.sleep(3)  # the DHT nodes find each other, A's pointer put

        status, _ = call("b", f"/api/lists/{pk}", {})
        check(status == 202, "B: subscribing by the key")
        got = wait("B takes version 1", lambda: [l for l in call("b", "/api/lists")[1]["lists"] if l["seq"] == 1 and l["net"].get("records_ready")])
        if not got:
            print(open(os.path.join(root, "b", "p2play.log")).read()[-3000:])
        check(bool(got) and not got[0]["own"], "B: version 1 from the DHT pointer and A's seed")
        res = call("b", "/api/catalog/search?q=network")[1]["results"]
        check([r["hash"] for r in res] == ["d" * 40], "B: the list searched")
        check(wait("B seeds", lambda: call("b", "/api/lists")[1]["lists"][0]["net"]["seeding"], 20), "B: seeding it")
        b_records_dir = os.path.join(root, "b", "lists", pk, "records")
        old_segments = [name for name in os.listdir(b_records_dir) if name.endswith(".z")]
        check(len(old_segments) == 1, "B: keeps the first immutable record segment locally")

        update_batch = "\n".join((
            json.dumps({"ih": "d" * 40, "title": "Revised Film", "tags": ["movie"]}),
            json.dumps({"ih": "e" * 40, "title": "Second Film"}),
        )) + "\n"
        call("a", f"/api/publish/{pk}/records", raw=update_batch.encode())
        check(call("a", f"/api/publish/{pk}/release", {})[1]["seq"] == 2, "A: version 2")
        time.sleep(2)
        call("b", f"/api/lists/{pk}", {})
        got = wait("B takes version 2", lambda: [l for l in call("b", "/api/lists")[1]["lists"] if l["seq"] == 2 and l["net"].get("records_ready")])
        check(bool(got) and call("b", "/api/catalog/search?q=second")[1]["results"] != [], "B: version 2 taken")
        revised = call("b", "/api/catalog/search?q=revised")[1]["results"]
        check([r["hash"] for r in revised] == ["d" * 40],
              "B: later segment fully replaces the prior record with the same infohash")
        check(call("b", "/api/catalog/search?q=network")[1]["results"] == [],
              "B: fields omitted from a replacement are not implicitly retained")
        with open(os.path.join(root, "b", "p2play.log"), encoding="utf-8") as log_file:
            b_log = log_file.read()
        check("preloaded_cached_record_segments=1" in b_log,
              "B: reuses the previously held immutable segment for the newer version")
        # the directory swarm: C holds no list, learns of this one and of its holders
        # Directory peers form a graph, not a guaranteed full mesh. C can learn via
        # one neighbor's transitive KNOWN snapshot; only HAVE contributes to holders.
        found = wait("C learns of the list", lambda: [l for l in call("c", "/api/lists/discover")[1]["lists"]
                                                       if l["pubkey"] == pk and l["seq"] == 2
                                                       and l.get("name") == "Net list" and l.get("known_by", 0) >= 1], 90)
        if not found:
            snapshot = call("c", "/api/lists/discover")[1]
            print("C discovery snapshot:", json.dumps(snapshot, ensure_ascii=False))
            log_path = os.path.join(root, "c", "p2play.log")
            if os.path.exists(log_path):
                with open(log_path, encoding="utf-8") as f:
                    print("C p2play.log:", f.read()[-3000:])
        check(bool(found) and not found[0]["subscribed"],
              "C: signed list discovery without subscribing")
        check(found[0]["holders"] <= found[0]["known_by"],
              "C: HAVE holders are included in knowledge, KNOWN does not inflate popularity")
        check(not call("c", "/api/lists")[1]["lists"],
              "C: discovery-only lookup does not import the version torrent")
        check(call("c", "/api/torrents")[0] == 404 and call("c", "/torrents", {"action": "list"})[0] == 404
              and call("c", "/api/catalog/search?q=x")[0] == 200, "C, a catalog node: no streamer, the catalog")
        print("PASS")
    finally:
        for p in procs:
            p.terminate()
            p.wait(timeout=20)
        if not os.environ.get("KEEP"):
            shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
