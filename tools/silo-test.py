#!/usr/bin/env python3
"""The native API (Silo's scheme under /api) against a local seeder, as the owner.

    tools/silo-test.py build-linux/src/app/p2playd

Checks: version and stats; add a magnet (JSON) and a .torrent (multipart); the status with
file indexes from 0 in TorrServer's order, names and file hashes; /api/stream with Range;
progress: a position, viewed past 90 %, "continue watching", the same position on MatriX's
/viewed (stage 2: the progress of one client seen on another); viewed by hand; the card
edited; the library and the progress after a restart; torrs:// (in the status, the
library exported and imported as .torrs, a link added on MatriX); delete.
"""

import hashlib
import importlib.util
import json
import os
import shutil
import sys
import tempfile
import urllib.request
import uuid

import libtorrent as lt

spec = importlib.util.spec_from_file_location("core_test", os.path.join(os.path.dirname(__file__), "core-test.py"))
ct = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ct)
check = ct.check


def call(path, body=None, method=None, headers=None, raw=None):
    data = raw if raw is not None else (None if body is None else json.dumps(body).encode())
    req = urllib.request.Request(ct.BASE + path, data=data, method=method or ("POST" if data is not None else "GET"))
    req.add_header("Authorization", "Bearer " + ct.OWNER_TOKEN)
    if body is not None:
        req.add_header("Content-Type", "application/json")
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            out = r.read()
            return r.status, json.loads(out) if out and r.headers.get("Content-Type", "").startswith("application/json") else out
    except urllib.error.HTTPError as e:
        out = e.read()
        return e.code, json.loads(out) if out else None


# Silo's torrshash.Pack: hash 5d902edb..., the title, a poster, category tv, size 12345678901,
# two trackers
SILO_TORRS = ("torrs://5V3A1LVmilvK3xy16gIRAZHoBqmfO4ZJRkLu0FqKxbaOTByQQ75puVqjMLNZSlYSKRl4Z1gafbO2eYk8VaSmksQ0m1r6Gz"
              "SlQzEBJqFBNsFsxkP4kyOJRXrUG1PBEMI3V36EkiUklKtSDyzw6r6u9NHANQaQxIJKLNkt8Onb0yUtWESOsaZQfDgBZnJpHTsmZ1c0Qj"
              "6v3wF1weo92UChPiSaCXUPLTfbQiqavSXIF5umBAtOqrC4Fuuytq3WF")


def idx_of(ti, path):
    fs = ct.torrent_layout(ti)
    return [fs.file_path(i).replace("\\", "/") for i in range(fs.num_files())].index(path)


def wait_files(ih):
    for _ in range(150):
        status, st = call(f"/api/torrents/{ih}")
        if status == 200 and st.get("file_stats"):
            return st
        ct.time.sleep(0.2)
    raise RuntimeError("no metadata")


def main():
    daemon = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build-linux/src/app/p2playd")
    root = tempfile.mkdtemp(prefix="p2play-silo-")
    data = os.path.join(root, "data")
    os.makedirs(data)
    proc = None
    try:
        folder, files = ct.make_payload(root)
        ti = ct.make_torrent(folder)
        ses, _ = ct.start_seeder(ti, os.path.join(root, "seed"))
        ih = str(ti.info_hashes().v1)
        magnet = lt.make_magnet_uri(ti) + f"&x.pe=127.0.0.1:{ct.SEED_PORT}"
        proc = ct.start_daemon(daemon, data)

        status, v = call("/api/system/version")
        check(status == 200 and v["version"] and v["torrent"].startswith("libtorrent"), "/api/system/version")
        status, me = call("/api/auth/me")
        check(status == 200 and me["id"] == "owner", "the owner's token")

        status, st = call("/api/torrents", {"link": magnet, "title": "Show", "poster": "http://p/1.jpg"})
        check(status == 201 and st["hash"] == ih and st["title"] == "Show", "add a magnet: 201, the status")
        st = wait_files(ih)
        names = [f["name"] for f in st["file_stats"]]
        check(names == ["e01.mkv", "e02.mkv", "e9.mkv", "e10.mkv", "notes.txt"]
              and [f["id"] for f in st["file_stats"]] == [idx_of(ti, f["path"])
                                                         for f in st["file_stats"]],
              "files in TorrServer's order, indexes as Silo (the torrent's, pad files counted)")
        S = {f["name"]: f["id"] for f in st["file_stats"]}
        M = {p.rsplit("/", 1)[1]: v[0] for p, v in ct.ts_ids(ti).items()}
        f0 = st["file_stats"][0]
        check(f0["file_hash"] == hashlib.sha256(f"e01.mkv:{len(files['e01.mkv'])}".encode()).hexdigest(),
              "file_hash as Silo")
        listed = call("/api/torrents")[1]["torrents"]
        check([t["hash"] for t in listed] == [ih] and listed[0]["poster"] == "http://p/1.jpg", "the library")
        # Silo's /api/events: the library first ("sync")
        with urllib.request.urlopen(ct.BASE + "/api/events?token=" + ct.OWNER_TOKEN, timeout=10) as r:
            event, line = r.readline().decode().strip(), r.readline().decode().strip()
        cards = json.loads(line.removeprefix("data:").strip())
        check(r.status == 200 and event == "event: sync" and [c["hash"] for c in cards] == [ih]
              and cards[0]["torrs"].startswith("torrs://"), "/api/events: the library as sync")
        check(call("/api/ui/menu")[1] == {"menu": []} and call("/api/system/torrent-config")[1]["preload_size"] > 0,
              "the menu and the engine config of Silo's client")

        status, body = call(f"/api/stream/{ih}/{S['e02.mkv']}", headers={"Range": "bytes=100-199"})
        check(status == 206 and body == files["e02.mkv"][100:200], "/api/stream: e02, a range")
        check(call(f"/api/stream/{ih}/99")[0] == 404, "/api/stream: a file out of range")

        # progress
        status, p = call(f"/api/torrents/{ih}/files/{S['e01.mkv']}/progress", {"position": 600, "duration": 2400}, "PUT")
        check(status == 200 and p["position"] == 600 and not p["viewed"], "a position")
        status, p = call(f"/api/torrents/{ih}/files/{S['e02.mkv']}/progress", {"position": 2200, "duration": 2400}, "PUT")
        check(status == 200 and p["viewed"], "past 90 %: viewed")
        st = call(f"/api/torrents/{ih}")[1]
        check(st["viewed_files"] == [S["e02.mkv"]]
              and {fp["id"]: fp["position"] for fp in st["file_progress"]} == {S["e01.mkv"]: 600, S["e02.mkv"]: 2200},
              "viewed_files and file_progress in the status")
        prog = call("/api/progress")[1]["progress"]
        check(len(prog) == 2 and {p["file_id"] for p in prog} == {S["e01.mkv"], S["e02.mkv"]} and prog[0]["title"] == "Show",
              "continue watching")
        viewed = ct.api("/viewed", {"action": "list", "hash": ih})
        check(sorted((v["file_index"], v["timecode"]) for v in viewed) == sorted([(M["e01.mkv"], 600), (M["e02.mkv"], 2200)]),
              "the same progress on MatriX's /viewed (MatriX's ids)")
        ct.api("/viewed", {"action": "set", "hash": ih, "file_index": M["e9.mkv"], "timecode": 30})
        st = call(f"/api/torrents/{ih}")[1]
        check(S["e9.mkv"] in st["viewed_files"] and any(fp["id"] == S["e9.mkv"] and fp["position"] == 30 for fp in st["file_progress"]),
              "MatriX's viewed seen on /api")

        check(call(f"/api/torrents/{ih}/files/{S['e01.mkv']}/viewed", {"viewed": True})[0] == 200
              and S["e01.mkv"] in call(f"/api/torrents/{ih}")[1]["viewed_files"], "viewed by hand")
        check(call(f"/api/torrents/{ih}/viewed", {"viewed": False})[0] == 200
              and call(f"/api/torrents/{ih}")[1]["viewed_files"] == [], "all files not viewed")
        check(call(f"/api/torrents/{ih}/meta", {"title": "Show 2", "poster": "", "category": "tv"}, "PUT")[0] == 200
              and call(f"/api/torrents/{ih}")[1]["category"] == "tv", "the card edited")
        check(call(f"/api/torrents/{ih}/files/{S['e01.mkv']}/preload", {})[0] == 200, "preload")
        check(call(f"/api/torrents/{ih}/peers")[0] == 200, "peers")

        # a .torrent, multipart, not saved
        boundary = uuid.uuid4().hex
        tfile = lt.bencode({b"info": lt.bdecode(ti.info_section())})
        other = lt.torrent_info(tfile)
        form = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"save_to_db\"\r\n\r\nfalse\r\n"
                f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.torrent\"\r\n"
                f"Content-Type: application/x-bittorrent\r\n\r\n").encode() + tfile + f"\r\n--{boundary}--\r\n".encode()
        status, st = call("/api/torrents", raw=form, headers={"Content-Type": f"multipart/form-data; boundary={boundary}"})
        check(status == 201 and st["hash"] == str(other.info_hashes().v1) and st.get("file_stats"),
              "add a .torrent (multipart): the files at once")

        # restart: the library, the progress
        ct.stop_daemon(proc)
        proc = ct.start_daemon(daemon, data)
        st = call(f"/api/torrents/{ih}")[1]
        check(st["title"] == "Show 2" and any(fp["id"] == S["e01.mkv"] and fp["position"] == 600 for fp in st["file_progress"]),
              "after a restart: the card and the progress")

        # torrs://: in the status, the library out and in, a link added on MatriX
        torrs = st.get("torrs", "")
        check(torrs.startswith("torrs://"), "torrs:// in the status")
        status, body = call("/api/library/export")
        lines = body.decode().split()
        check(status == 200 and lines == [torrs], "the library exported as .torrs")
        check(call(f"/api/torrents/{ih}", method="DELETE")[0] == 200 and call("/api/torrents")[1]["torrents"] == []
              and call(f"/api/torrents/{ih}")[0] == 404, "delete")
        status, out = call("/api/library/import", raw=body, headers={"Content-Type": "text/plain"})
        listed = call("/api/torrents")[1]["torrents"]
        check(status == 200 and out["count"] == 1 and [t["hash"] for t in listed] == [ih]
              and listed[0]["title"] == "Show 2" and listed[0]["category"] == "tv", "imported: the card back")
        check(call("/api/library/import", raw=body, headers={"Content-Type": "text/plain"})[1]["count"] == 0,
              "imported again: nothing new")
        check(listed[0]["stat"] == 5, "an imported card is not opened (as Silo)")
        check(call(f"/api/torrents/{ih}", method="DELETE")[0] == 200, "delete again")
        st = ct.api("/torrents", {"action": "add", "link": torrs, "save_to_db": True})
        check(st["hash"] == ih and st["title"] == "Show 2", "MatriX add: a torrs:// link")
        ct.api("/torrents", {"action": "rem", "hash": ih})
        check(call("/api/torrents")[1]["torrents"] == [], "removed on MatriX")
        check(call("/api/progress")[1]["progress"] == [], "the progress of a deleted torrent gone")

        # a link packed by Silo's internal/torrshash (Go): read as Silo writes it
        status, out = call("/api/library/import", raw=SILO_TORRS.encode(), headers={"Content-Type": "text/plain"})
        card = call("/api/torrents/5d902edb06f1ca0d2a713ce3cdd395b75d36f735")[1]
        check(status == 200 and out["count"] == 1 and card["title"] == "Сериал S01 (2024) WEB-DL 1080p"
              and card["poster"] == "https://image.tmdb.org/t/p/w500/abc.jpg" and card["category"] == "tv"
              and card["torrent_size"] == 12345678901, "a torrs:// link of Silo")
        check(call("/api/torrents/5d902edb06f1ca0d2a713ce3cdd395b75d36f735", method="DELETE")[0] == 200, "its card removed")
        print("PASS")
        del ses
    finally:
        if proc:
            proc.terminate()
            proc.wait(timeout=20)
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
