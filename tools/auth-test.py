#!/usr/bin/env python3
"""p2playd with users (auth.json and MatriX's accs.db): what needs a user and what does not.

    tools/auth-test.py build-linux/src/app/p2playd

Checks: /echo public; the API without a user: 401 with WWW-Authenticate (MatriX); Basic
(auth.json and accs.db users), ?token=, Bearer and the login cookie accepted; a wrong
password refused; /api/auth/login and /api/auth/me; a known torrent played without a user
(players open these links without credentials), an unknown link refused.
"""

import base64
import importlib.util
import json
import os
import shutil
import sys
import tempfile
import urllib.request

import libtorrent as lt

spec = importlib.util.spec_from_file_location("core_test", os.path.join(os.path.dirname(__file__), "core-test.py"))
ct = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ct)
check = ct.check


def call(path, body=None, headers=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(ct.BASE + path, data=data, method="POST" if body is not None else "GET")
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


def basic(user, password):
    return {"Authorization": "Basic " + base64.b64encode(f"{user}:{password}".encode()).decode()}


def main():
    daemon = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build-linux/src/app/p2playd")
    root = tempfile.mkdtemp(prefix="p2play-auth-")
    data = os.path.join(root, "data")
    os.makedirs(data)
    with open(os.path.join(data, "auth.json"), "w") as f:
        json.dump({"users": [{"name": "owner", "password": "secret", "token": "ts_owner0000000000000000000"}]}, f)
    with open(os.path.join(data, "accs.db"), "w") as f:
        json.dump({"matrix": "pass"}, f)
    proc = None
    try:
        folder, files = ct.make_payload(root)
        ti = ct.make_torrent(folder)
        ses, _ = ct.start_seeder(ti, os.path.join(root, "seed"))
        ih = str(ti.info_hashes().v1)
        magnet = lt.make_magnet_uri(ti) + f"&x.pe=127.0.0.1:{ct.SEED_PORT}"
        proc = ct.start_daemon(daemon, data)

        check(call("/echo")[0] == 200, "/echo without a user")
        status, headers, _ = call("/torrents", {"action": "list"})
        check(status == 401 and "Basic" in headers.get("WWW-Authenticate", ""), "list without a user: 401, WWW-Authenticate")
        check(call("/torrents", {"action": "list"}, basic("owner", "wrong"))[0] == 401, "a wrong password refused")
        check(call("/torrents", {"action": "list"}, basic("owner", "secret"))[0] == 200, "Basic (auth.json)")
        check(call("/torrents", {"action": "list"}, basic("matrix", "pass"))[0] == 200, "Basic (MatriX accs.db)")
        check(call("/torrents?token=ts_owner0000000000000000000", {"action": "list"})[0] == 200, "?token=")
        check(call("/torrents", {"action": "list"}, {"Authorization": "Bearer ts_owner0000000000000000000"})[0] == 200,
              "Bearer")

        status, headers, body = call("/api/auth/login", {"username": "matrix", "password": "pass"})
        token = json.loads(body).get("token", "") if status == 200 else ""
        check(status == 200 and token.startswith("ts_") and "p2play_token=" + token in headers.get("Set-Cookie", ""),
              "login: a token (generated for the accs.db user) and its cookie")
        check(call("/api/auth/login", {"username": "matrix", "password": "nope"})[0] == 401, "login: wrong password")
        status, _, body = call("/api/auth/me", headers={"Cookie": "p2play_token=" + token})
        check(status == 200 and json.loads(body)["user"]["name"] == "matrix", "me with the cookie")
        users = json.load(open(os.path.join(data, "auth.json")))["users"]
        check(any(u["name"] == "matrix" and u["token"] == token for u in users), "auth.json: the token kept")

        auth = basic("owner", "secret")
        check(call("/torrents", {"action": "add", "link": magnet, "save_to_db": True}, auth)[0] == 200, "add with a user")
        for _ in range(100):
            st = json.loads(call("/torrents", {"action": "get", "hash": ih}, auth)[2])
            if st.get("file_stats"):
                break
            ct.time.sleep(0.2)
        status, _, body = call(f"/stream/e01.mkv?link={ih}&index=1&play", headers={"Range": "bytes=0-99"})
        check(status == 206 and body == files["e01.mkv"][:100], "a known torrent played without a user")
        check(call(f"/play/{ih}/1", headers={"Range": "bytes=0-9"})[0] == 206, "/play without a user")
        check(call(f"/stream?link={ih}&index=1&stat")[0] == 401, "stat without a user: 401")
        other = "magnet:?xt=urn:btih:" + "1" * 40
        check(call(f"/stream?link={other}&index=1&play")[0] == 401, "an unknown link without a user: 401")
        # each user has their own viewed files
        call("/viewed", {"action": "set", "hash": ih, "file_index": 1, "timecode": 10}, basic("owner", "secret"))
        call("/viewed", {"action": "set", "hash": ih, "file_index": 2, "timecode": 20}, basic("matrix", "pass"))
        mine = json.loads(call("/viewed", {"action": "list"}, basic("owner", "secret"))[2])
        theirs = json.loads(call("/viewed", {"action": "list"}, basic("matrix", "pass"))[2])
        check([(v["file_index"], v["timecode"]) for v in mine] == [(1, 10)]
              and [(v["file_index"], v["timecode"]) for v in theirs] == [(2, 20)], "viewed: a list a user")
        print("PASS")
        del ses
    finally:
        if proc:
            proc.terminate()
            proc.wait(timeout=20)
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
