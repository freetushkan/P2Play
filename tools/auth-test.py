#!/usr/bin/env python3
"""Native P2Play authentication and durable API-token tests.

    tools/auth-test.py build/src/app/p2playd

Checks stable tokens across login/restart, owner and user token reset/revocation,
Bearer and share-query authentication, rank checks, and removal of legacy routes.
"""
import importlib.util
import json
import os
import shutil
import sys
import subprocess
import time
import tempfile
import urllib.error
import urllib.request

spec = importlib.util.spec_from_file_location("test_utils", os.path.join(os.path.dirname(__file__), "test_utils.py"))
test_utils = importlib.util.module_from_spec(spec)
spec.loader.exec_module(test_utils)


def call(path, body=None, token=None, method=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(test_utils.BASE + path, data=data, method=method or ("POST" if body is not None else "GET"))
    if token:
        req.add_header("Authorization", "Bearer " + token)
    if body is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=30) as response:
            raw = response.read()
            return response.status, dict(response.headers), json.loads(raw) if raw else None
    except urllib.error.HTTPError as error:
        raw = error.read()
        try:
            value = json.loads(raw) if raw else None
        except ValueError:
            value = raw.decode(errors="replace")
        return error.code, dict(error.headers), value


def check(condition, message):
    print(("OK   " if condition else "FAIL ") + message)
    if not condition:
        raise RuntimeError(message)


def main():
    daemon = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/src/app/p2playd")
    root = tempfile.mkdtemp(prefix="p2play-auth-")
    data = os.path.join(root, "data")
    os.makedirs(data)
    owner_token = "p2p-owner0000000000000000000000"
    with open(os.path.join(data, "auth.json"), "w", encoding="utf-8") as file:
        json.dump({"users": [{"id": "owner", "username": "owner", "password": "secret",
                              "api_token": owner_token, "rank": 100}]}, file)
    proc = None
    try:
        log = open(os.path.join(data, "stdout.log"), "a", encoding="utf-8")
        proc = subprocess.Popen([daemon, "--data", data, "--port", str(test_utils.HTTP_PORT)], stdout=log, stderr=subprocess.STDOUT)
        for _ in range(100):
            try:
                if call("/api/v1/system/ping")[0] == 200:
                    break
            except Exception:
                pass
            time.sleep(0.1)
        check(call("/api/v1/system/ping")[0] == 200, "native ping is public")
        check(call("/api/v1/auth/me")[0] == 401, "private API requires authentication")
        check(call("/api/auth/login", {"username": "owner", "password": "secret"})[0] == 404,
              "unversioned login route is removed")
        status, headers, login = call("/api/v1/auth/login", {"username": "owner", "password": "secret"})
        check(status == 200 and login["token"] == owner_token, "login returns the stored token")
        status, _, again = call("/api/v1/auth/login", {"username": "owner", "password": "secret"})
        check(status == 200 and again["token"] == owner_token, "repeated login does not rotate the token")
        status, _, me = call("/api/v1/auth/me", token=owner_token)
        check(status == 200 and me["id"] == "owner" and me["rank"] == 100 and me["api_token"] == "",
              "account endpoint does not expose the token")
        status, _, users = call("/api/v1/users", token=owner_token)
        check(status == 200 and len(users["users"]) == 1 and users["users"][0]["api_token"] == "",
              "user list never exposes API tokens")

        status, _, created = call("/api/v1/users", {"username": "guest", "password": "pass", "rank": 10},
                                  token=owner_token)
        check(status == 201 and created["api_token"].startswith("p2p-"), "new user receives one stable token")
        guest_token = created["api_token"]
        check(call("/api/v1/auth/me", token=guest_token)[0] == 200, "guest token authenticates")
        check(call("/api/v1/users", token=guest_token)[0] == 403, "ordinary user cannot manage users")

        status, _, reset = call("/api/v1/users/" + created["id"] + "/regenerate-token", {},
                                token=owner_token)
        new_guest_token = reset["token"]
        check(status == 200 and new_guest_token != guest_token, "administrator explicitly resets a user's token")
        check(call("/api/v1/auth/me", token=guest_token)[0] == 401, "old user token is immediately revoked")
        check(call("/api/v1/auth/me", token=new_guest_token)[0] == 200, "new user token works")

        status, _, reset_owner = call("/api/v1/users/owner/regenerate-token", {}, token=owner_token)
        new_owner_token = reset_owner["token"]
        check(status == 200 and new_owner_token != owner_token, "owner can reset their own token")
        check(call("/api/v1/auth/me", token=owner_token)[0] == 401, "old owner token is immediately revoked")
        check(call("/api/v1/auth/me", token=new_owner_token)[0] == 200, "new owner token works")

        check(call("/torrents", token=new_owner_token)[0] == 404, "MatriX routes are removed")
        check(call("/api/torrents", token=new_owner_token)[0] == 404, "unversioned Silo-compatible routes are removed")
        check(call("/api/v1/auth/me?token=" + new_owner_token)[0] == 200,
              "share-query token authentication works")
        test_utils.stop_daemon(proc)
        proc = None

        log = open(os.path.join(data, "stdout.log"), "a", encoding="utf-8")
        proc = subprocess.Popen([daemon, "--data", data, "--port", str(test_utils.HTTP_PORT)], stdout=log, stderr=subprocess.STDOUT)
        for _ in range(100):
            try:
                if call("/api/v1/system/ping")[0] == 200:
                    break
            except Exception:
                pass
            time.sleep(0.1)
        status, _, login_after_restart = call("/api/v1/auth/login", {"username": "owner", "password": "secret"})
        check(status == 200 and login_after_restart["token"] == new_owner_token,
              "token survives a server restart and login reuses it")
        print("PASS")
    finally:
        if proc:
            proc.terminate()
            proc.wait(timeout=20)
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
