#!/usr/bin/env python3
"""The web interface of the core: the Silo plugin's pages (plugins/mediamesh) served at /ui/.

    tools/ui-test.py build-linux/src/app/p2playd [--browser]

Checks: a browser at / goes to /ui/, a client gets the node's card; the three files are the
plugin's own, byte for byte, with the plugin's policy (CSP, nosniff); the API behind them asks
a login. With --browser (Playwright and Chromium needed): log in on the page, search a list in
Cyrillic, add a result, and the torrent is in the owner's library.
"""

import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

BASE = "http://127.0.0.1:8191"
OWNER_PASSWORD = "test"
OWNER_TOKEN = "ts_test00000000000000000000"


def check(cond, what):
    print(("OK   " if cond else "FAIL ") + what)
    if not cond:
        raise RuntimeError(what)


def start_daemon(daemon, data):
    if not os.path.exists(os.path.join(data, "auth.json")):
        with open(os.path.join(data, "auth.json"), "w") as f:
            json.dump({"users": [{"username": "owner", "password": OWNER_PASSWORD,
                                  "api_token": OWNER_TOKEN}]}, f)
    log = open(os.path.join(data, "stdout.log"), "a")
    proc = subprocess.Popen([daemon, "--data", data, "--port", "8191"],
                            stdout=log, stderr=subprocess.STDOUT)
    log.close()
    for _ in range(100):
        try:
            if get("/echo")[0] == 200:
                return proc
        except (OSError, urllib.error.URLError):
            pass
        time.sleep(0.1)
    raise RuntimeError("p2playd did not start")


def stop_daemon(proc):
    proc.terminate()
    proc.wait(timeout=20)


PLUGIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "plugins", "mediamesh")


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def get(path, headers=None):
    req = urllib.request.Request(BASE + path, headers=headers or {})
    try:
        with urllib.request.build_opener(NoRedirect).open(req, timeout=10) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


def call(path, body=None):
    req = urllib.request.Request(BASE + path, data=None if body is None else json.dumps(body).encode(),
                                 method="POST" if body is not None else "GET")
    req.add_header("Authorization", "Bearer " + OWNER_TOKEN)
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read() or b"null")


def browser():
    from playwright.sync_api import sync_playwright
    pk = call("/api/publish", {"name": "UI list", "description": "for the page"})["pubkey"]
    ih = "d" * 40
    req = urllib.request.Request(BASE + f"/api/publish/{pk}/records", method="POST",
                                 data=json.dumps({"ih": ih, "title": "Мультфильм Ёжик (1975)", "tags": ["cartoon"]}, ensure_ascii=False).encode())
    req.add_header("Authorization", "Bearer " + OWNER_TOKEN)
    urllib.request.urlopen(req, timeout=30).read()
    call(f"/api/publish/{pk}/release", {})
    with sync_playwright() as p:
        b = p.chromium.launch()
        page = b.new_page()
        errors = []
        page.on("pageerror", lambda e: errors.append(str(e)))
        page.goto(BASE + "/")
        check(page.url.endswith("/ui/"), "the browser at /ui/")
        page.wait_for_selector("#login-view:not([hidden])")
        page.fill("#password", OWNER_PASSWORD)
        page.click("#login-form button")
        page.wait_for_selector("#search-view:not([hidden])")
        check(page.title() == "MediaMesh · P2Play", "logged in on the page")
        page.fill("#query", "ежик")
        page.click("#search-form button")
        page.wait_for_selector("#results .row")
        check("Ёжик" in page.inner_text("#results"), "found in Cyrillic, ё as е")
        page.click("#results .row button")
        page.wait_for_selector("#results .row button:disabled")
        check(any(t["hash"] == ih for t in call("/api/torrents")["torrents"]), "added: in the owner's library")
        page.click("[data-view=lists]")
        page.wait_for_selector("#lists .row")
        check("UI list" in page.inner_text("#lists"), "the lists view")
        check(not errors, "no script errors: " + "; ".join(errors))
        b.close()


def main():
    daemon = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else "build-linux/src/app/p2playd")
    root = tempfile.mkdtemp(prefix="p2play-ui-")
    proc = start_daemon(daemon, root)
    try:
        status, headers, _ = get("/", {"Accept": "text/html,application/xhtml+xml"})
        check(status == 302 and headers.get("Location") == "/ui/", "a browser at / goes to /ui/")
        status, _, body = get("/")
        check(status == 200 and json.loads(body)["name"] == "P2Play", "a client at / gets the node's card")
        check(get("/ui")[0] == 301, "/ui to /ui/")
        for path, name in (("/ui/", "index.html"), ("/ui/index.html", "index.html"), ("/ui/app.js", "app.js"), ("/ui/style.css", "style.css")):
            status, headers, body = get(path)
            with open(os.path.join(PLUGIN, name), "rb") as f:
                same = f.read() == body
            check(status == 200 and same and "frame-ancestors" in headers.get("Content-Security-Policy", "")
                  and headers.get("X-Content-Type-Options") == "nosniff", path + ": the plugin's " + name)
        check(get("/ui/service.js")[0] == 404 and get("/ui/info.yaml")[0] == 404, "the plugin's server side not served")
        check(get("/api/auth/me")[0] == 401, "the API behind the page asks a login")
        if "--browser" in sys.argv:
            browser()
        print("PASS")
    finally:
        stop_daemon(proc)


if __name__ == "__main__":
    main()
