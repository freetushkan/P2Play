#!/usr/bin/env python3
"""Local p2spike check without DHT: a seeder on 127.0.0.1, p2spike gets the magnet
(metadata over BEP 9 from the seeder), then byte ranges over HTTP are compared with
the source file.

    pip install libtorrent
    tools/local-test.py build-linux/src/spike/p2spike [p2spike options: --cache 16, --storage disk]
"""

import hashlib
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

import libtorrent as lt

SEED_PORT = 6901
SPIKE_PORT = 6902
HTTP_PORT = 8195
SIZE = 48 * 1024 * 1024


def make_payload(root):
    folder = os.path.join(root, "seed", "Test Release")
    os.makedirs(folder)
    with open(os.path.join(folder, "readme.txt"), "w") as f:
        f.write("not the stream\n")
    path = os.path.join(folder, "movie.mkv")
    rnd = random.Random(42)
    with open(path, "wb") as f:
        left = SIZE
        while left:
            n = min(left, 1 << 20)
            f.write(rnd.randbytes(n))
            left -= n
    return folder, path


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


def fetch(url, start=None, end=None, timeout=40):
    req = urllib.request.Request(url)
    if start is not None:
        req.add_header("Range", f"bytes={start}-" + ("" if end is None else str(end)))
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, dict(r.headers), r.read()


def main():
    spike = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build-linux/src/spike/p2spike")
    extra = sys.argv[2:]
    root = tempfile.mkdtemp(prefix="p2spike-")
    proc = None
    try:
        folder, movie = make_payload(root)
        ti = make_torrent(folder)
        ses, _ = start_seeder(ti, os.path.join(root, "seed"))
        magnet = lt.make_magnet_uri(ti)
        print("magnet:", magnet)

        env = dict(os.environ, P2SPIKE_DATA=os.path.join(root, "spike"))
        proc = subprocess.Popen([spike, magnet, "--peer", f"127.0.0.1:{SEED_PORT}",
                                 "--port", str(SPIKE_PORT), "--http", str(HTTP_PORT)] + extra,
                                env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

        base = f"http://127.0.0.1:{HTTP_PORT}"
        deadline = time.time() + 60
        while True:
            try:
                status, _, _ = fetch(base + "/stream", 0, 0, timeout=5)
                if status == 206:
                    break
            except Exception:
                pass
            if time.time() > deadline:
                raise RuntimeError("stream not ready in 60 s")
            time.sleep(0.5)

        with open(movie, "rb") as f:
            data = f.read()

        checks = [(0, 1023), (SIZE - 4096, None), (SIZE // 2 + 17, SIZE // 2 + 300000), (5 * 1024 * 1024 + 1, None)]
        for start, end in checks:
            status, headers, body = fetch(base + "/stream", start, end)
            want = data[start:(SIZE if end is None else end + 1)]
            ok = status == 206 and body == want
            print(f"range {start}-{end}: {status} {len(body)} bytes {'OK' if ok else 'MISMATCH'}"
                  f" ({headers.get('Content-Range')})")
            if not ok:
                raise RuntimeError("range mismatch")

        status, _, body = fetch(base + "/stream")
        ok = status == 200 and hashlib.sha1(body).digest() == hashlib.sha1(data).digest()
        print(f"full file: {status} {len(body)} bytes {'OK' if ok else 'MISMATCH'}")
        if not ok:
            raise RuntimeError("full file mismatch")

        print("status:", fetch(base + "/status")[2].decode().strip())
        print("PASS")
        del ses
    finally:
        if proc:
            proc.terminate()
            try:
                out, _ = proc.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                out, _ = proc.communicate()
            print("---- p2spike log ----")
            print(out)
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
