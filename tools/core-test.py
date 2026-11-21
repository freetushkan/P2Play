#!/usr/bin/env python3
"""p2playd against the TorrServer MatriX API as pPlay and Lampa use it, without DHT: a
seeder on 127.0.0.1 with a two-file torrent, p2playd in a scratch directory.

    pip install libtorrent
    tools/core-test.py build-linux/src/app/p2playd [--disk | --cache16]

--cache16: a 16 MB RAM cache, smaller than the 24 MB file streamed whole: pieces dropped
and downloaded again.

Checks: /echo; /torrents add (magnet, save_to_db) / get / list (data with the files, as
pPlay reads it) / set; /stream ?link&index&play byte ranges and the whole file, /play;
?stat and ?preload; /viewed set / list / rem with timecode; a restart: the torrent in the
library ("in db"), streamed again from its kept metadata; /settings; /playlist; rem.
"""

import hashlib
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

import libtorrent as lt

SEED_PORT = 6911
HTTP_PORT = 8191
BIG = 24 * 1024 * 1024


def make_payload(root):
    folder = os.path.join(root, "seed", "Show S01")
    os.makedirs(folder)
    rnd = random.Random(7)
    files = {}
    for name, size in (("e01.mkv", BIG), ("e02.mkv", BIG // 3 + 12345), ("notes.txt", 100)):
        data = rnd.randbytes(size)
        with open(os.path.join(folder, name), "wb") as f:
            f.write(data)
        files[name] = data
    return folder, files


def make_torrent(folder):
    fs = lt.file_storage()
    lt.add_files(fs, folder)
    ct = lt.create_torrent(fs, 256 * 1024)
    lt.set_piece_hashes(ct, os.path.dirname(folder))
    return lt.torrent_info(ct.generate())


def start_seeder(ti, save_path):
    ses = lt.session({
        "listen_interfaces": f"127.0.0.1:{SEED_PORT}",
        "enable_dht": False, "enable_lsd": False, "enable_upnp": False, "enable_natpmp": False,
        "allow_multiple_connections_per_ip": True,
    })
    h = ses.add_torrent({"ti": ti, "save_path": save_path})
    for _ in range(100):
        if h.status().state == lt.torrent_status.seeding:
            return ses, h
        time.sleep(0.1)
    raise RuntimeError("seeder did not finish checking")


BASE = f"http://127.0.0.1:{HTTP_PORT}"


def http(path, body=None, rng=None, timeout=60):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(BASE + path, data=data, method="POST" if body is not None else "GET")
    if body is not None:
        req.add_header("Content-Type", "application/json")
    if rng:
        req.add_header("Range", "bytes=" + rng)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


def api(path, body):
    status, _, out = http(path, body)
    if status != 200:
        raise RuntimeError(f"{path} {body}: {status} {out[:200]}")
    return json.loads(out) if out else None


def check(cond, what):
    print(("OK   " if cond else "FAIL ") + what)
    if not cond:
        raise RuntimeError(what)


def start_daemon(daemon, data):
    log = open(os.path.join(data, "stdout.log"), "a")
    proc = subprocess.Popen([daemon, "--data", data, "--port", str(HTTP_PORT)], stdout=log, stderr=subprocess.STDOUT)
    for _ in range(100):
        try:
            if http("/echo", timeout=2)[0] == 200:
                return proc
        except Exception:
            pass
        time.sleep(0.1)
    raise RuntimeError("p2playd did not start")


def stop_daemon(proc):
    proc.terminate()
    proc.wait(timeout=20)


def main():
    daemon = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build-linux/src/app/p2playd")
    disk = "--disk" in sys.argv[2:]
    small = "--cache16" in sys.argv[2:]
    root = tempfile.mkdtemp(prefix="p2playd-")
    data = os.path.join(root, "data")
    os.makedirs(data)
    if disk or small:
        with open(os.path.join(data, "settings.json"), "w") as f:
            json.dump({"UseDisk": True} if disk else {"CacheSize": 16 * 1024 * 1024}, f)
    proc = None
    try:
        folder, files = make_payload(root)
        ti = make_torrent(folder)
        ses, seed = start_seeder(ti, os.path.join(root, "seed"))
        ih = str(ti.info_hashes().v1)  # v1, as TorrServer (a hybrid torrent: info_hash() is v2)
        # the seeder as the only source: p2playd finds it through the magnet's x.pe
        magnet = lt.make_magnet_uri(ti) + f"&x.pe=127.0.0.1:{SEED_PORT}"

        proc = start_daemon(daemon, data)
        status, _, echo = http("/echo")
        check(status == 200 and b"MatriX" in echo, f"/echo: {echo.decode()}")

        st = api("/torrents", {"action": "add", "link": magnet, "title": "Show", "poster": "http://p/1.jpg",
                               "data": "{\"lampa\":true}", "save_to_db": True})
        check(st["hash"] == ih, "add: hash " + st["hash"])

        for _ in range(100):
            st = api("/torrents", {"action": "get", "hash": ih})
            if st.get("file_stats"):
                break
            time.sleep(0.2)
        names = [f["path"] for f in st.get("file_stats", [])]
        check([f["id"] for f in st["file_stats"]] == [1, 2, 3], "get: file ids from 1: " + ", ".join(names))
        check(st["title"] == "Show" and st["poster"] == "http://p/1.jpg", "get: title and poster kept")

        lst = api("/torrents", {"action": "list"})
        d = json.loads(lst[0]["data"])
        check(d.get("lampa") is True and len(d["TorrServer"]["Files"]) == 3, "list: data with TorrServer.Files (pPlay)")

        index = {os.path.basename(f["path"]): f["id"] for f in st["file_stats"]}
        e01, e02 = files["e01.mkv"], files["e02.mkv"]
        for rng, want in (("0-1023", e01[:1024]), (f"{BIG - 5000}-", e01[BIG - 5000:]),
                          (f"{BIG // 2}-{BIG // 2 + 400000}", e01[BIG // 2:BIG // 2 + 400001]), ("-777", e01[-777:])):
            status, headers, body = http(f"/stream/e01.mkv?link={ih}&index={index['e01.mkv']}&play", rng=rng)
            check(status == 206 and body == want, f"stream e01 range {rng}: {status}, {len(body)} bytes")
        status, headers, body = http(f"/stream?link={ih}&index={index['e02.mkv']}&play")
        check(status == 200 and hashlib.sha1(body).digest() == hashlib.sha1(e02).digest(),
              f"stream e02 whole: {len(body)} bytes, {headers.get('Content-Type')}")
        status, _, body = http(f"/stream?link={ih}&index={index['e01.mkv']}&play")
        check(status == 200 and hashlib.sha1(body).digest() == hashlib.sha1(e01).digest(),
              f"stream e01 whole: {len(body)} bytes")
        status, _, body = http(f"/play/{ih}/{index['e01.mkv']}", rng="100-199")
        check(status == 206 and body == e01[100:200], "/play/<hash>/<id> range")

        status, _, body = http(f"/stream?link={ih}&index={index['e01.mkv']}&preload&stat")
        st = json.loads(body)
        check(status == 200 and st["preload_size"] > 0 and st["preloaded_bytes"] == st["preload_size"],
              f"preload + stat: {st['preloaded_bytes']}/{st['preload_size']} bytes, stat {st['stat_string']}")

        api("/viewed", {"action": "set", "hash": ih, "file_index": 1, "timecode": 123.5})
        api("/viewed", {"action": "set", "hash": ih, "file_index": 2})
        v = api("/viewed", {"action": "list", "hash": ih})
        check(sorted((x["file_index"], x["timecode"]) for x in v) == [(1, 123.5), (2, 0)], f"viewed: {v}")
        api("/viewed", {"action": "rem", "hash": ih, "file_index": 2})
        v = api("/viewed", {"action": "list"})
        check([(x["file_index"], x["timecode"]) for x in v] == [(1, 123.5)], "viewed: rem one file")

        api("/torrents", {"action": "set", "hash": ih, "title": "Show (renamed)"})
        s = api("/settings", {"action": "get"})
        check(s["CacheSize"] > 0 and "ReaderReadAHead" in s and s["UseDisk"] == disk, "settings get")
        api("/settings", {"action": "set", "sets": {"TorrentDisconnectTimeout": 10, "FriendlyName": "test"}})
        s = api("/settings", {"action": "get"})
        check(s["TorrentDisconnectTimeout"] == 10 and s["FriendlyName"] == "test", "settings set")

        status, _, m3u = http(f"/playlist?hash={ih}")
        lines = [l for l in m3u.decode().splitlines() if l.startswith("http")]
        check(status == 200 and len(lines) == 2 and all("&play" in l for l in lines), "playlist: 2 media files")

        # a restart: the library survives, the torrent is opened again from its metadata
        stop_daemon(proc)
        check(os.path.getsize(os.path.join(data, "torrents", ih + ".resume")) > 0,
              "resume data kept (metadata with the v2 piece layers, peers)")
        del ses  # no seeder: the kept metadata is needed for the file list
        proc = start_daemon(daemon, data)
        lst = api("/torrents", {"action": "list"})
        check(len(lst) == 1 and lst[0]["stat"] == 5 and lst[0]["title"] == "Show (renamed)"
              and len(lst[0]["file_stats"]) == 3, f"restart: in db, \"{lst[0]['title']}\", files kept")
        check(api("/viewed", {"action": "list", "hash": ih})[0]["timecode"] == 123.5, "restart: viewed kept")
        ses, seed = start_seeder(ti, os.path.join(root, "seed"))
        try:
            status, _, body = http(f"/stream?link={ih}&index={index['e01.mkv']}&play", rng="5000000-5099999")
        except Exception as e:
            print("stream failed:", e, "- torrent:", api("/torrents", {"action": "get", "hash": ih}))
            raise
        check(status == 206 and body == e01[5000000:5100000], "restart: stream from the library")

        api("/torrents", {"action": "rem", "hash": ih})
        check(api("/torrents", {"action": "list"}) == [], "rem: library empty")
        print("PASS")
        del ses
    finally:
        if proc:
            try:
                stop_daemon(proc)
            except Exception:
                proc.kill()
            with open(os.path.join(data, "stdout.log")) as f:
                print("---- p2playd log ----")
                print(f.read()[-6000:])
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
