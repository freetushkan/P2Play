#!/usr/bin/env python3
"""Shared helpers for standalone P2Play integration tests."""

import json
import os
import subprocess
import time
import urllib.error
import urllib.request

HTTP_PORT = 8191
BASE = f"http://127.0.0.1:{HTTP_PORT}"
OWNER_PASSWORD = "test"
OWNER_TOKEN = "ts_test00000000000000000000"


def check(cond, what):
    print(("OK   " if cond else "FAIL ") + what)
    if not cond:
        raise RuntimeError(what)


def start_daemon(daemon, data, users=True):
    if users and not os.path.exists(os.path.join(data, "auth.json")):
        with open(os.path.join(data, "auth.json"), "w", encoding="utf-8") as f:
            json.dump({"users": [{"username": "owner", "password": OWNER_PASSWORD,
                                  "api_token": OWNER_TOKEN}]}, f)
    log = open(os.path.join(data, "stdout.log"), "a", encoding="utf-8")
    proc = subprocess.Popen([daemon, "--data", data, "--port", str(HTTP_PORT)],
                            stdout=log, stderr=subprocess.STDOUT)
    url = BASE + "/api/v1/system/ping"
    for _ in range(100):
        try:
            with urllib.request.urlopen(url, timeout=2) as response:
                if response.status == 200:
                    return proc
        except (OSError, urllib.error.HTTPError):
            pass
        time.sleep(0.1)
    proc.terminate()
    proc.wait(timeout=20)
    raise RuntimeError("p2playd did not start")


def stop_daemon(proc):
    proc.terminate()
    proc.wait(timeout=20)
