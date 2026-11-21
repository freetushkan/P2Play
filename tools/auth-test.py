#!/usr/bin/env python3
"""p2playd with users: one set of users (auth.json), each API with its own scheme.

    tools/auth-test.py build-linux/src/app/p2playd

Checks: /echo and the ping public; MatriX routes: 401 with WWW-Authenticate, Basic accepted,
a wrong password and a token refused; /api: 401 without a token, the token (?token=, Bearer,
the login cookie) accepted, Basic refused; a plain password in auth.json hashed at the start;
users created by the owner log in on both APIs; a known torrent played without a user, an
unknown link refused; each user has their own library and viewed files.
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


def call(path, body=None, headers=None, method=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(ct.BASE + path, data=data, method=method or ("POST" if body is not None else "GET"))
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


def basic(user, password):
    return {"Authorization": "Basic " + base64.b64encode(f"{user}:{password}".encode()).decode()}


def bearer(token):
    return {"Authorization": "Bearer " + token}


def main():
    daemon = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build-linux/src/app/p2playd")
    root = tempfile.mkdtemp(prefix="p2play-auth-")
    data = os.path.join(root, "data")
    os.makedirs(data)
    owner_token = "ts_owner0000000000000000000"
    with open(os.path.join(data, "auth.json"), "w") as f:
        json.dump({"users": [{"name": "owner", "password": "secret", "token": owner_token}]}, f)
    proc = None
    try:
        folder, files = ct.make_payload(root)
        ti = ct.make_torrent(folder)
        ses, _ = ct.start_seeder(ti, os.path.join(root, "seed"))
        ih = str(ti.info_hashes().v1)
        magnet = lt.make_magnet_uri(ti) + f"&x.pe=127.0.0.1:{ct.SEED_PORT}"
        proc = ct.start_daemon(daemon, data)
        owner = basic("owner", "secret")

        check(call("/echo")[0] == 200 and call("/api/system/ping")[0] == 200, "/echo and the ping without a user")
        # pPlay's discovery: port 8090, /echo answered with 200, its first 40 characters without '<';
        # TorrServe for Android takes a MatriX server by the "MatriX." prefix
        status, _, echo = call("/echo")
        check(status == 200 and echo.decode().startswith("MatriX.") and "<" not in echo.decode()[:40],
              "found by pPlay's discovery and TorrServe with users set: " + echo.decode())
        stored = json.load(open(os.path.join(data, "auth.json")))["users"][0]
        check(stored["password_hash"].startswith("sha256$") and "password" not in stored, "auth.json: the password hashed")
        check(json.loads(call("/api/system/auth", headers=bearer(owner_token))[2]) == {"required": True, "password_set": True},
              "/api/system/auth")

        # MatriX: Basic only
        status, headers, _ = call("/torrents", {"action": "list"})
        check(status == 401 and "Basic" in headers.get("WWW-Authenticate", ""), "MatriX without a user: 401, WWW-Authenticate")
        check(call("/torrents", {"action": "list"}, basic("owner", "wrong"))[0] == 401, "MatriX: a wrong password refused")
        check(call("/torrents", {"action": "list"}, owner)[0] == 200, "MatriX: Basic")
        check(call("/torrents", {"action": "list"}, bearer(owner_token))[0] == 401
              and call(f"/torrents?token={owner_token}", {"action": "list"})[0] == 401, "MatriX: a token refused")

        # /api: tokens only
        status, headers, _ = call("/api/torrents")
        check(status == 401 and "WWW-Authenticate" not in headers, "/api without a token: 401, no WWW-Authenticate")
        check(call("/api/torrents", headers=owner)[0] == 401, "/api: Basic refused")
        check(call("/api/torrents", headers=bearer(owner_token))[0] == 200, "/api: Bearer")
        check(call(f"/api/torrents?token={owner_token}")[0] == 200, "/api: ?token=")
        status, headers, body = call("/api/auth/login", {"username": "owner", "password": "secret"})
        check(status == 200 and json.loads(body)["token"] == owner_token
              and "silo_token=" + owner_token in headers.get("Set-Cookie", ""), "login: the token and the silo_token cookie")
        check(call("/api/auth/login", {"username": "owner", "password": "nope"})[0] == 401, "login: a wrong password")
        status, _, body = call("/api/auth/me", headers={"Cookie": "silo_token=" + owner_token})
        me = json.loads(body)
        check(status == 200 and me["id"] == "owner" and me["rank"] == 100 and "password_hash" not in me, "me with the cookie")

        # a user made by the owner, on both APIs
        status, _, body = call("/api/users", {"username": "kid", "password": "pass", "rank": 10}, bearer(owner_token))
        kid = json.loads(body)
        check(status == 201 and kid["api_token"].startswith("ts_"), "a user created")
        kid_token = kid["api_token"]
        check(call("/api/users", headers=bearer(kid_token))[0] == 403, "a user cannot list the users")
        check(call("/api/users", {"username": "x", "password": "y", "rank": 50}, bearer(kid_token))[0] == 403,
              "a user cannot create one of a higher rank")
        listed = json.loads(call("/api/users", headers=bearer(owner_token))[2])["users"]
        check(len(listed) == 2 and all(u["api_token"] == "" for u in listed), "users listed without their tokens")
        check(call("/torrents", {"action": "list"}, basic("kid", "pass"))[0] == 200, "the user: Basic on MatriX")

        check(call("/torrents", {"action": "add", "link": magnet, "save_to_db": True}, owner)[0] == 200, "add with a user")
        for _ in range(100):
            st = json.loads(call("/torrents", {"action": "get", "hash": ih}, owner)[2])
            if st.get("file_stats"):
                break
            ct.time.sleep(0.2)
        e01 = ct.ts_ids(ti)["Show S01/e01.mkv"][0]
        status, _, body = call(f"/stream/e01.mkv?link={ih}&index={e01}&play", headers={"Range": "bytes=0-99"})
        check(status == 206 and body == files["e01.mkv"][:100], "a known torrent played without a user")
        check(call(f"/play/{ih}/{e01}", headers={"Range": "bytes=0-9"})[0] == 206, "/play without a user")
        check(call(f"/stream?link={ih}&index=1&stat")[0] == 401, "stat without a user: 401")
        other = "magnet:?xt=urn:btih:" + "1" * 40
        check(call(f"/stream?link={other}&index=1&play")[0] == 401, "an unknown link without a user: 401")

        # each user has their own library and viewed files
        kid_list = json.loads(call("/torrents", {"action": "list"}, basic("kid", "pass"))[2])
        check(all(t["hash"] != ih or t["stat"] != 5 for t in kid_list)
              and json.loads(call("/api/torrents", headers=bearer(kid_token))[2])["torrents"] == [],
              "the owner's torrent is not in the user's library")
        call("/viewed", {"action": "set", "hash": ih, "file_index": 1, "timecode": 10}, owner)
        call("/viewed", {"action": "set", "hash": ih, "file_index": 2, "timecode": 20}, basic("kid", "pass"))
        mine = json.loads(call("/viewed", {"action": "list"}, owner)[2])
        theirs = json.loads(call("/viewed", {"action": "list"}, basic("kid", "pass"))[2])
        check([(v["file_index"], v["timecode"]) for v in mine] == [(1, 10)]
              and [(v["file_index"], v["timecode"]) for v in theirs] == [(2, 20)], "viewed: a list a user")

        check(call(f"/api/users/{kid['id']}", headers=bearer(kid_token), method="DELETE")[0] == 403,
              "a user cannot delete himself")
        check(call(f"/api/users/{kid['id']}", headers=bearer(owner_token), method="DELETE")[0] == 200
              and call("/api/torrents", headers=bearer(kid_token))[0] == 401, "a deleted user's token refused")
        check(call("/api/users/owner", headers=bearer(owner_token), method="DELETE")[0] == 403, "the owner stays")
        ct.stop_daemon(proc)
        proc = None

        # a new server: authorization required from the start; the first owner login sets the password
        fresh = os.path.join(root, "fresh")
        os.makedirs(fresh)
        proc = ct.start_daemon(daemon, fresh, users=False)
        check(call("/torrents", {"action": "list"})[0] == 401 and call("/api/torrents")[0] == 401,
              "a new server: both APIs need a login")
        check(json.loads(call("/api/system/auth", headers=bearer(json.load(open(os.path.join(fresh, "auth.json")))["users"][0]["api_token"]))[2])["password_set"] is False,
              "no password yet")
        check(call("/torrents", {"action": "list"}, basic("owner", "first"))[0] == 200,
              "the first Basic as the owner sets the password")
        check(call("/torrents", {"action": "list"}, basic("owner", "other"))[0] == 401
              and call("/api/auth/login", {"username": "owner", "password": "first"})[0] == 200,
              "one password on both APIs")
        print("PASS")
        del ses
    finally:
        if proc:
            proc.terminate()
            proc.wait(timeout=20)
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
