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
library ("in db"), streamed again from its kept metadata; /settings; /playlist; lists
while the torrent is closed and opened again; rem.
"""

import gc
import hashlib
import json
import os
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
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
    # e9 / e10: TorrServer's order (numbers as numbers), not the torrent's
    for name, size in (("e01.mkv", BIG), ("e02.mkv", BIG // 3 + 12345), ("e10.mkv", 3000), ("e9.mkv", 2000),
                       ("notes.txt", 100)):
        data = rnd.randbytes(size)
        with open(os.path.join(folder, name), "wb") as f:
            f.write(data)
        files[name] = data
    return folder, files


def ts_less(a, b):
    """TorrServer's CompareStrings (server/utils/strings.go), on bytes."""
    a, b = a.encode(), b.encode()
    n = 0
    while n < min(len(a), len(b)) and a[n] == b[n]:
        n += 1
    while n > 0 and chr(a[n - 1]).isdigit():
        n -= 1
    ra, rb = a[n:], b[n:]

    def num(s):
        k = 0
        while k < len(s) and chr(s[k]).isdigit():
            k += 1
        return int(s[:k]) if k else None

    na, nb = num(ra), num(rb)
    if na is not None and nb is not None:
        return ra < rb if na == nb else na < nb
    if na is not None:
        return True
    if nb is not None:
        return False
    return ra < rb


def torrent_layout(ti):
    """Use the current torrent_info layout API, retaining compatibility with older wheels."""
    layout = getattr(ti, "layout", None)
    return layout() if callable(layout) else ti.files()


def ts_ids(ti):
    """The ids TorrServer MatriX gives the files: every file, pad files too, sorted by path."""
    import functools
    fs = torrent_layout(ti)
    paths = [fs.file_path(i).replace("\\", "/") for i in range(fs.num_files())]
    order = sorted(range(len(paths)), key=functools.cmp_to_key(
        lambda x, y: -1 if ts_less(paths[x], paths[y]) else (1 if ts_less(paths[y], paths[x]) else 0)))
    pad = [bool(fs.file_flags(i) & lt.file_storage.flag_pad_file) for i in range(fs.num_files())]
    return {paths[i]: (k + 1, i) for k, i in enumerate(order) if not pad[i]}


def make_torrent(folder):
    # libtorrent 2.1 replaces add_files(file_storage) and the file_storage
    # create_torrent constructor with list_files(create_file_entry[]).
    list_files = getattr(lt, "list_files", None)
    if callable(list_files):
        ct = lt.create_torrent(list_files(folder), 256 * 1024)
    else:
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
    params = lt.add_torrent_params()
    params.ti = ti
    params.save_path = save_path
    h = ses.add_torrent(params)
    for _ in range(100):
        # A seeding torrent is not sufficient: if the requested listen port was
        # occupied, libtorrent can fall back to another port while x.pe still
        # points at SEED_PORT. Fail here instead of waiting 60 seconds for a peer.
        if h.status().state == lt.torrent_status.seeding and ses.is_listening():
            actual_port = ses.listen_port()
            if actual_port != SEED_PORT:
                raise RuntimeError(f"seeder requested port {SEED_PORT}, but libtorrent listened on {actual_port}")
            return ses, h
        time.sleep(0.1)
    raise RuntimeError(f"seeder did not finish checking/listening (port={ses.listen_port()})")


def wait_seeder_port_closed(timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.settimeout(0.2)
            if probe.connect_ex(("127.0.0.1", SEED_PORT)) != 0:
                return
        time.sleep(0.05)
    raise RuntimeError(f"previous test seeder still listens on 127.0.0.1:{SEED_PORT}")


BASE = f"http://127.0.0.1:{HTTP_PORT}"
# the tests' owner (authorization is required): written into a data directory without users
OWNER_PASSWORD = "test"
OWNER_TOKEN = "ts_test00000000000000000000"
OWNER_BASIC = "Basic " + __import__("base64").b64encode(f"owner:{OWNER_PASSWORD}".encode()).decode()


def http(path, body=None, rng=None, timeout=60, auth=True):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(BASE + path, data=data, method="POST" if body is not None else "GET")
    if auth:
        req.add_header("Authorization", OWNER_BASIC)
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


def start_daemon(daemon, data, users=True):
    if users and not os.path.exists(os.path.join(data, "auth.json")):
        with open(os.path.join(data, "auth.json"), "w") as f:
            json.dump({"users": [{"username": "owner", "password": OWNER_PASSWORD, "api_token": OWNER_TOKEN}]}, f)
    log = open(os.path.join(data, "stdout.log"), "a")
    proc = subprocess.Popen([daemon, "--data", data, "--port", str(HTTP_PORT)], stdout=log, stderr=subprocess.STDOUT)
    for _ in range(100):
        try:
            if http("/echo", timeout=2, auth=False)[0] == 200:
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
    # This test has exactly one intended peer, supplied by the magnet's x.pe.
    # Do not let public DHT/MediaMesh add unrelated peers to resume data across restarts.
    settings = {"DisableDHT": True, "DisableMeshDirectory": True}
    if disk:
        settings["UseDisk"] = True
    elif small:
        settings["CacheSize"] = 16 * 1024 * 1024
    with open(os.path.join(data, "settings.json"), "w") as f:
        json.dump(settings, f)
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

        status, _, native_version = http("/api/p2play/version", auth=False)
        version_data = json.loads(native_version) if status == 200 else {}
        check(status == 200 and version_data == {"name": "P2Play", "version": "0.1"},
              f"public native P2Play version endpoint: HTTP {status} {version_data}")
        status, _, legacy_ping = http("/api/system/ping", auth=False)
        check(status == 200 and json.loads(legacy_ping) == {"status": "ok"},
              "legacy Silo ping response remains unchanged")

        st = api("/torrents", {"action": "add", "link": magnet, "title": "Show", "poster": "http://p/1.jpg",
                               "data": "{\"lampa\":true}", "save_to_db": True})
        check(st["hash"] == ih, "add: hash " + st["hash"])

        for _ in range(100):
            st = api("/torrents", {"action": "get", "hash": ih})
            if st.get("file_stats"):
                break
            time.sleep(0.2)
        names = [f["path"] for f in st.get("file_stats", [])]
        expect = ts_ids(ti)
        check([f["id"] for f in st["file_stats"]] == sorted(v[0] for v in expect.values())
              and all(expect[f["path"]][0] == f["id"] for f in st["file_stats"]),
              "get: file ids as TorrServer gives them (pad files counted): "
              + ", ".join(f"{f['id']} {f['path']}" for f in st["file_stats"]))
        check([os.path.basename(n) for n in names] == ["e01.mkv", "e02.mkv", "e9.mkv", "e10.mkv", "notes.txt"],
              "get: TorrServer's file order (e9 before e10)")
        check(st["title"] == "Show" and st["poster"] == "http://p/1.jpg", "get: title and poster kept")

        lst = api("/torrents", {"action": "list"})
        d = json.loads(lst[0]["data"])
        check(d.get("lampa") is True and len(d["TorrServer"]["Files"]) == 5, "list: data with TorrServer.Files (pPlay)")

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

        api("/torrents", {"action": "set", "hash": ih, "title": "Show (renamed)", "poster": ""})
        # as MatriX: /stream with a poster fills the empty one (kept in the library)
        http(f"/stream?link={ih}&index=1&stat&poster=http%3A%2F%2Fp%2F2.jpg")
        check(api("/torrents", {"action": "get", "hash": ih})["poster"] == "http://p/2.jpg", "stream: the poster filled")
        s = api("/settings", {"action": "get"})
        check(s["CacheSize"] > 0 and "ReaderReadAHead" in s and s["UseDisk"] == disk
              and s["DisableDHT"] and s["DisableMeshDirectory"], "settings get: isolated single-peer test")
        api("/settings", {"action": "set", "sets": {"TorrentDisconnectTimeout": 10, "FriendlyName": "test"}})
        s = api("/settings", {"action": "get"})
        check(s["TorrentDisconnectTimeout"] == 10 and s["FriendlyName"] == "test", "settings set")

        status, _, m3u = http(f"/playlist?hash={ih}")
        lines = [l for l in m3u.decode().splitlines() if l.startswith("http")]
        check(status == 200 and len(lines) == 4 and all("&play" in l for l in lines), "playlist: 4 media files")

        # a restart: the library survives, the torrent is opened again from its metadata
        stop_daemon(proc)
        check(os.path.getsize(os.path.join(data, "torrents", ih + ".resume")) > 0,
              "resume data kept (metadata with the v2 piece layers, peers)")
        # Destroy both the session and its torrent handle, then confirm the fixed
        # x.pe port is free. Recreate the seeder before reopening P2Play: libtorrent
        # attempts the saved peer as soon as the resumed torrent is activated. If the
        # daemon starts while x.pe is down, the failed connection can stall this test.
        del seed
        del ses
        gc.collect()
        wait_seeder_port_closed()
        ses, seed = start_seeder(ti, os.path.join(root, "seed"))
        proc = start_daemon(daemon, data)
        lst = api("/torrents", {"action": "list"})
        check(len(lst) == 1 and lst[0]["stat"] == 5 and lst[0]["title"] == "Show (renamed)"
              and lst[0]["poster"] == "http://p/2.jpg" and len(lst[0]["file_stats"]) == 5,
              f"restart: in db, \"{lst[0]['title']}\", poster and files kept")
        check(api("/viewed", {"action": "list", "hash": ih})[0]["timecode"] == 123.5, "restart: viewed kept")
        try:
            status, _, body = http(f"/stream?link={ih}&index={index['e01.mkv']}&play", rng="5000000-5099999")
        except Exception as e:
            print("stream failed:", e, "- torrent:", api("/torrents", {"action": "get", "hash": ih}))
            raise
        check(status == 206 and body == e01[5000000:5100000], "restart: stream from the library")

        # lists while the torrent is closed and opened again (a closed handle throws): the
        # daemon must stay up
        stop = []

        def lister():
            while not stop and proc.poll() is None:
                try:
                    http("/torrents", {"action": "list"}, timeout=5)
                except Exception:
                    pass

        listers = [threading.Thread(target=lister) for _ in range(6)]
        for t in listers:
            t.start()
        for _ in range(80):
            if proc.poll() is not None:
                break
            http("/torrents", {"action": "drop", "hash": ih}, timeout=10)
            http("/torrents", {"action": "add", "link": magnet}, timeout=10)
        stop.append(True)
        for t in listers:
            t.join()
        check(proc.poll() is None, "list during drop / add: the daemon is up")

        api("/torrents", {"action": "rem", "hash": ih})
        check(api("/torrents", {"action": "list"}) == [], "rem: library empty")

        status, _, body = http("/download/3", rng="1048576-2097151")
        check(status == 206 and len(body) == 1048576 and body.count(0) == len(body), "/download speed test, a range")
        check(http("/storage/settings")[0] == 200, "/storage/settings")
        check(http("/shutdown")[0] == 200, "/shutdown answered")
        proc.wait(timeout=20)
        check(proc.returncode == 0, "/shutdown: the daemon stopped")
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
