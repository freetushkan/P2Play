#!/usr/bin/env python3
"""MediaMesh lists, the local half: a list published on one node, taken by another, searched.

    tools/mesh-test.py build-linux/src/app/p2playd

Checks: a list made (its key on the node); records checked (a bad one refused) and released
as signed versions; the search (words in any case, Cyrillic too, a tag filter) with a
torrs:// link a result; a removal in a later version; another node takes the version
directory: the signature, the segment hashes and seq checked (tampered copies refused, an
older version refused); the list kept after a restart, dropped by hand.
"""

import importlib.util
import json
import os
import shutil
import sys
import tempfile
import urllib.parse
import urllib.request

spec = importlib.util.spec_from_file_location("core_test", os.path.join(os.path.dirname(__file__), "core-test.py"))
ct = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ct)
check = ct.check


def call(path, body=None, method=None, raw=None):
    data = raw if raw is not None else (None if body is None else json.dumps(body).encode())
    url = ct.BASE + urllib.parse.quote(path, safe="/?=&,%")
    req = urllib.request.Request(url, data=data, method=method or ("POST" if data is not None else "GET"))
    req.add_header("Authorization", "Bearer " + ct.OWNER_TOKEN)
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


IH1, IH2, IH3 = "a" * 40, "b" * 40, "c" * 64
RECORDS = "\n".join(json.dumps(r, ensure_ascii=False) for r in [
    {"ih": IH1, "title": "Сериал Тест S01 1080p", "size": 1234567, "tags": ["tv", "WEB-DL"], "trackers": ["udp://t.example:1337"]},
    {"ih": IH2.upper(), "title": "Some Movie (2024)", "files": 3, "date": 1700000000, "tags": ["movie"]},
    {"ih": IH3, "title": "Ёлки — v2 only"},
])


def main():
    daemon = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build-linux/src/app/p2playd")
    root = tempfile.mkdtemp(prefix="p2play-mesh-")
    a, b = os.path.join(root, "a"), os.path.join(root, "b")
    os.makedirs(a)
    os.makedirs(b)
    proc = None
    try:
        # This test covers local catalog APIs and manual import only. Keep its
        # nodes outside public DHT and the directory rendezvous.
        for data in (a, b):
            with open(os.path.join(data, "settings.json"), "w") as f:
                json.dump({"DisableDHT": True, "DisableMeshDirectory": True}, f)
        proc = ct.start_daemon(daemon, a)
        check(call("/api/lists")[1]["lists"] == [], "fresh server has no catalog lists")
        check(call("/api/catalog/search?q=anything")[1]["results"] == [], "search is empty before a list is added")
        bad_name = call("/api/publish", {"name": "Bad" + chr(0) + "Name"})
        check(bad_name[0] == 400, "a catalog name containing NUL is rejected")
        status, out = call("/api/publish", {"name": "Test 0 list", "description": "for the test"})
        pk = out.get("pubkey", "")
        check(status == 201 and len(pk) == 64 and os.path.exists(os.path.join(a, "keys", pk + ".seed")), "a list made, its key kept")
        status, out = call(f"/api/publish/{pk}/records", raw=b'{"ih": "xyz", "title": "bad"}\n')
        check(status == 400 and "line 1" in out["error"], "a bad record refused")
        status, out = call(f"/api/publish/{pk}/records", raw=RECORDS.encode())
        check(status == 200 and out["count"] == 3 and out["pending"] == 3, "records waiting")
        status, out = call(f"/api/publish/{pk}/release", {})
        check(status == 200 and out["seq"] == 1 and out["records"] == 3, "version 1 released")
        check(call(f"/api/publish/{pk}/release", {})[0] == 400, "nothing to release")

        res = call("/api/catalog/search?q=" + urllib.parse.quote("сериал 1080P"))[1]["results"]
        check([r["hash"] for r in res] == [IH1] and res[0]["torrs"].startswith("torrs://") and res[0]["lists"] == [pk],
              "search: Cyrillic in another case, a torrs:// link")
        res = call("/api/catalog/search?q=" + urllib.parse.quote("сериал WEB-DL"))[1]["results"]
        check([r["hash"] for r in res] == [IH1],
              "multi-keyword search: terms can match the title and tags")
        res = call("/api/catalog/search?q=" + urllib.parse.quote("сериал 4K"))[1]["results"]
        check(res == [], "multi-keyword search requires every query term to match")
        scoped = call("/api/catalog/search?q=" + urllib.parse.quote("сериал") + "&lists=" + pk)[1]["results"]
        check([r["hash"] for r in scoped] == [IH1], "search scoped to a selected catalog list")
        scoped = call("/api/catalog/search?q=" + urllib.parse.quote("сериал") + "&lists=" + pk + "," + "f" * 64)[1]["results"]
        check([r["hash"] for r in scoped] == [IH1], "search scoped to any of several selected lists")
        check(call("/api/catalog/search?q=" + urllib.parse.quote("сериал") + "&lists=" + "f" * 64)[1]["results"] == [],
              "an unknown selected list does not broaden the search");
        res = call("/api/catalog/search?q=" + urllib.parse.quote("елки"))[1]["results"]
        check([r["hash"] for r in res] == [IH3] and "torrs" not in res[0], "search: ё as е; no torrs:// for a v2-only hash")
        res = call("/api/catalog/search?q=&tag=MOVIE")[1]["results"]
        check([r["hash"] for r in res] == [IH2], "search: a tag, hashes in lower case")
        status, rec = call(f"/api/catalog/{IH1}")
        check(status == 200 and rec["size"] == 1234567 and rec["sources"][0]["name"] == "Test list", "a record and its list")

        call(f"/api/publish/{pk}/records", raw=json.dumps({"del": IH2}).encode())
        status, out = call(f"/api/publish/{pk}/release", {})
        check(status == 200 and out["seq"] == 2 and out["records"] == 2
              and call("/api/catalog/search?q=movie")[1]["results"] == [], "version 2: a removal")

        replacement = json.dumps({
            "ih": IH1, "title": "Curated Catalog Entry", "date": 1700000100,
            "tags": ["clean"],
        }, ensure_ascii=False) + "\n"
        status, out = call(f"/api/publish/{pk}/replace", raw=replacement.encode())
        check(status == 200 and out["seq"] == 3 and out["records"] == 1
              and out["input_records"] == 1, "version 3: full replacement")
        check([r["hash"] for r in call("/api/catalog/search?q=curated")[1]["results"]] == [IH1]
              and call("/api/catalog/search?q=" + urllib.parse.quote("елки"))[1]["results"] == []
              and call("/api/catalog/search?q=сериал")[1]["results"] == [],
              "replacement keeps included row and removes omitted rows")
        check(call(f"/api/publish/{pk}/status")[1]["pubkey"] == pk,
              "replacement preserves the original public key")
        src = os.path.join(root, "published-v3")
        shutil.copytree(os.path.join(a, "lists", pk), src)

        status, out = call(f"/api/publish/{pk}/replace", raw=b"")
        check(status == 200 and out["seq"] == 4 and out["records"] == 0
              and out["input_records"] == 0
              and call("/api/catalog/search?q=curated")[1]["results"] == [],
              "empty replacement clears the catalog")

        ct.stop_daemon(proc)
        proc = None

        # another node takes the saved non-empty version 3
        proc = ct.start_daemon(daemon, b)
        bad = os.path.join(root, "bad")
        shutil.copytree(src, bad)
        record_segments = [x for x in os.listdir(os.path.join(bad, "records")) if x.endswith(".z")]
        check(len(record_segments) == 1, "replacement produced one fresh immutable record segment")
        with open(os.path.join(bad, "records", record_segments[0]), "r+b") as f:
            f.seek(10)
            byte = f.read(1)
            f.seek(10)
            f.write(bytes([byte[0] ^ 1]))
        status, out = call("/api/lists/import", {"dir": bad})
        check(status == 400 and "hash mismatch" in out["error"], "a tampered segment refused")
        shutil.rmtree(bad)
        shutil.copytree(src, bad)
        manifest = open(os.path.join(bad, "manifest.ben"), "rb").read()
        open(os.path.join(bad, "manifest.ben"), "wb").write(manifest.replace(b"4:name11:Test 0 list", b"4:name11:Evil list 0"))
        status, out = call("/api/lists/import", {"dir": bad})
        check(status == 400 and "signature" in out["error"], "a tampered manifest refused")
        status, _ = call("/api/lists/import", {"dir": src})
        lists = call("/api/lists")[1]["lists"]
        check(status == 200 and len(lists) == 1 and lists[0]["seq"] == 3 and not lists[0]["own"], "the version taken")
        check(call("/api/lists/import", {"dir": src})[0] == 400, "the same version again refused")
        check([r["hash"] for r in call("/api/catalog/search?q=curated")[1]["results"]] == [IH1], "searched on the other node")
        ct.stop_daemon(proc)
        proc = ct.start_daemon(daemon, b)
        check(len(call("/api/lists")[1]["lists"]) == 1, "kept after a restart")
        check(call(f"/api/publish/{pk}/status")[0] == 404, "no publishing without the key")
        check(call(f"/api/lists/{pk}", method="DELETE")[0] == 200 and call("/api/lists")[1]["lists"] == []
              and call("/api/catalog/search?q=" + urllib.parse.quote("тест"))[1]["results"] == [], "the list dropped")
        print("PASS")
    finally:
        if proc:
            proc.terminate()
            proc.wait(timeout=20)
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
