#!/usr/bin/env python3
"""The web interface of the core: the Silo plugin's pages (plugins/mediamesh) served at /ui/.

    tools/ui-test.py build-linux/src/app/p2playd [--browser]

Checks: a browser at / goes to /ui/, a client gets the node's card; the three files are the
plugin's own, byte for byte, with the plugin's policy (CSP, nosniff); the API behind them asks
a login. With --browser (Playwright and Chromium needed): log in on the page, search a list in
Cyrillic, add a result, and the torrent is in the owner's library.
"""

import hashlib
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
            if get("/api/system/ping")[0] == 200:
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



def api_v1(method, path, body=None, token=OWNER_TOKEN, content_type=None):
    if isinstance(body, dict):
        payload = json.dumps(body, ensure_ascii=False).encode()
        content_type = content_type or "application/json"
    else:
        payload = body
    req = urllib.request.Request(BASE + path, data=payload, method=method)
    if token:
        req.add_header("Authorization", "Bearer " + token)
    if content_type:
        req.add_header("Content-Type", content_type)
    try:
        with urllib.request.urlopen(req, timeout=30) as response:
            raw = response.read()
            return response.status, json.loads(raw) if raw else None
    except urllib.error.HTTPError as error:
        raw = error.read()
        return error.code, json.loads(raw) if raw else None


def multipart(fields, file_name, file_data):
    boundary = "p2play-ui-test-boundary"
    chunks = []
    for name, value in fields.items():
        chunks.extend([
            ("--" + boundary + "\r\n").encode(),
            ('Content-Disposition: form-data; name="' + name + '"\r\n\r\n').encode(),
            value.encode(), b"\r\n"
        ])
    chunks.extend([
        ("--" + boundary + "\r\n").encode(),
        ('Content-Disposition: form-data; name="file"; filename="' + file_name + '"\r\n').encode(),
        b"Content-Type: application/x-bittorrent\r\n\r\n",
        file_data, b"\r\n",
        ("--" + boundary + "--\r\n").encode()
    ])
    return b"".join(chunks), "multipart/form-data; boundary=" + boundary


def native_library():
    check(api_v1("GET", "/api/v1/library/torrents", token="")[0] == 401,
          "native library API requires a session token")
    check(api_v1("GET", "/api/v1/library/torrents")[0] == 200,
          "native library API accepts the Silo-compatible Bearer token")

    magnet_hash = "a" * 40
    magnet = "magnet:?xt=urn:btih:" + magnet_hash + "&dn=The%20Torrent"
    status, added = api_v1("POST", "/api/v1/torrents", {
        "link": magnet, "title": "Ёжик", "category": "Animation"
    })
    check(status == 201 and added["torrent"]["hash"] == magnet_hash,
          "native API adds a magnet to the current user's library")

    second_hash = "b" * 40
    status, added_second = api_v1("POST", "/api/v1/torrents", {
        "link": "magnet:?xt=urn:btih:" + second_hash,
        "title": "Alpha", "category": "Drama"
    })
    check(status == 201 and added_second["torrent"]["category"] == "Drama",
          "native API accepts a second magnet and its category")

    piece = hashlib.sha1(b"x").digest()
    info = (b"d6:lengthi1e4:name8:one.binx12:piece lengthi16384e6:pieces20:" +
            piece + b"e")
    torrent_file = b"d4:info" + info + b"e"
    body, content_type = multipart({"title": "Uploaded", "category": "Files"},
                                   "uploaded.torrent", torrent_file)
    status, uploaded = api_v1("POST", "/api/v1/torrents", body, content_type=content_type)
    file_hash = uploaded["torrent"]["hash"] if uploaded and uploaded.get("torrent") else ""
    check(status == 201 and len(file_hash) == 40 and uploaded["torrent"]["category"] == "Files",
          "native API accepts a binary multipart .torrent upload")

    status, searched = api_v1("GET", "/api/v1/library/torrents?q=%D0%B5%D0%B6%D0%B8%D0%BA")
    check(status == 200 and [x["hash"] for x in searched["items"]] == [magnet_hash],
          "library search folds Russian case and ё/е")
    status, category = api_v1("GET", "/api/v1/library/torrents?category=drama")
    check(status == 200 and len(category["items"]) == 1 and category["items"][0]["hash"] == second_hash,
          "library category filtering is case-insensitive")
    status, uncategorized = api_v1("GET", "/api/v1/library/torrents?category=none")
    check(status == 200 and len(uncategorized["items"]) == 0,
          "library supports the uncategorized filter")
    status, ordered = api_v1("GET", "/api/v1/library/torrents?sort=title&order=asc")
    titles = [item["title"] for item in ordered["items"]]
    check(status == 200 and titles == sorted(titles, key=str.casefold),
          "library title sort orders the complete result set")
    status, page = api_v1("GET", "/api/v1/library/torrents?sort=size&order=desc&offset=1&limit=1")
    check(status == 200 and len(page["items"]) == 1 and page["total"] == 3,
          "library pagination is applied after sorting and reports the full total")

    status, changed = api_v1("PUT", "/api/v1/torrents/" + second_hash, {"category": "Series"})
    check(status == 200 and changed["torrent"]["category"] == "Series",
          "library metadata can be updated via the native REST route")
    status, categories = api_v1("GET", "/api/v1/library/categories")
    check(status == 200 and set(categories["categories"]) == {"Animation", "Files", "Series"},
          "library category endpoint returns the current user's distinct categories")

    owner_user = call("/api/users", {"username": "library-user", "password": "test-user", "rank": 10})
    user_token = owner_user["api_token"]
    status, own_before = api_v1("GET", "/api/v1/library/torrents", token=user_token)
    check(status == 200 and own_before["total"] == 0,
          "a second user starts with an isolated empty library")
    status, shared = api_v1("POST", "/api/v1/torrents", {
        "link": magnet, "title": "Ignored duplicate title", "category": "Ignored"
    }, token=user_token)
    check(status == 201 and shared["torrent"]["hash"] == magnet_hash,
          "the same infohash can be saved by a second user")
    status, _ = api_v1("DELETE", "/api/v1/torrents/" + magnet_hash)
    check(status == 200, "owner removes a torrent from only their library")
    status, owner_after = api_v1("GET", "/api/v1/library/torrents")
    status2, user_after = api_v1("GET", "/api/v1/library/torrents", token=user_token)
    check(status == 200 and magnet_hash not in [x["hash"] for x in owner_after["items"]] and
          status2 == 200 and magnet_hash in [x["hash"] for x in user_after["items"]],
          "removing one user's library entry preserves the other user's entry")
    status, _ = api_v1("DELETE", "/api/v1/torrents/" + magnet_hash, token=user_token)
    check(status == 200, "the second user can remove their own library entry")
    status, own_after = api_v1("GET", "/api/v1/library/torrents", token=user_token)
    check(status == 200 and own_after["total"] == 0,
          "the second user's library is empty after their own removal")


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
        check(not page.locator("#library-tab").is_hidden(), "core-host exposes the Library tab")
        check(page.title() == "MediaMesh · P2Play", "logged in on the page")
        page.fill("#query", "ежик")
        page.click("#search-form button")
        page.wait_for_selector("#results .row")
        check("Ёжик" in page.inner_text("#results"), "found in Cyrillic, ё as е")
        page.click("#results .row button")
        page.wait_for_selector("#results .row button:disabled")
        check(any(t["hash"] == ih for t in call("/api/torrents")["torrents"]), "added: in the owner's library")
        page.click("[data-view=library]")
        page.wait_for_selector("#library-view:not([hidden])")
        page.fill("#library-link", "magnet:?xt=urn:btih:" + "e" * 40)
        page.fill("#library-title", "Browser Library Entry")
        page.fill("#library-new-category", "Browser")
        page.click("#library-add-form button")
        page.wait_for_selector("#library-rows .library-row")
        check("Browser Library Entry" in page.inner_text("#library-rows"),
              "browser Library tab adds a magnet to the user library")
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
        check(get("/api/p2play/version")[0] == 200 and
              json.loads(get("/api/p2play/version")[2])["name"] == "P2Play",
              "native public P2Play identity is available without MatriX")
        matrix_status, matrix_headers, _ = get("/torrents", {"Authorization": "Basic dGVzdDp0ZXN0"})
        check(matrix_status == 404 and "WWW-Authenticate" not in matrix_headers,
              "MatriX routes and Basic challenge are disabled in the standard build")
        check(get("/echo")[0] == 404, "MatriX discovery route is absent in the standard build")
        check(get("/api/auth/me")[0] == 401, "the API behind the page asks a login")
        native_library()
        if "--browser" in sys.argv:
            browser()
        print("PASS")
    finally:
        stop_daemon(proc)


if __name__ == "__main__":
    main()
