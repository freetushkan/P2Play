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
    info = (b"d6:lengthi1e4:name7:one.bin" +
            b"12:piece lengthi16384e6:pieces20:" + piece + b"e")
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

    status, changed = api_v1("PUT", "/api/v1/torrents/" + second_hash,
                             {"title": "Updated Alpha", "category": "Series"})
    check(status == 200 and changed["torrent"]["title"] == "Updated Alpha" and
          changed["torrent"]["category"] == "Series",
          "library title and category can be updated via the native REST route")
    status, detail = api_v1("GET", "/api/v1/torrents/" + file_hash)
    check(status == 200 and "file_stats" in detail["details"],
          "native detailed torrent route exposes file metadata and per-user playback state")
    status, file_list = api_v1("GET", "/api/v1/torrents/" + file_hash + "/files")
    check(status == 200, "native file-list route responds successfully")
    check(len(file_list.get("files", [])) == 1, "native file-list route returns one file")
    check(file_list["files"][0].get("name") == "one.bin",
          "native file-list route returns the correct torrent-relative filename")
    file_id = file_list["files"][0]["id"]
    status, marked = api_v1("PUT", "/api/v1/torrents/" + file_hash + "/files/" + str(file_id) + "/viewed",
                            {"viewed": True})
    check(status == 200 and marked["viewed"],
          "native API can mark an individual file as watched")
    status, progress = api_v1("PUT", "/api/v1/torrents/" + file_hash + "/files/" + str(file_id) + "/progress",
                              {"position": 5.0, "duration": 120.0})
    check(status == 200 and progress["position"] == 5.0,
          "native API stores per-user file playback progress")
    status, continued = api_v1("GET", "/api/v1/progress")
    check(status == 200 and any(x["hash"] == file_hash and x["file_id"] == file_id
                                for x in continued["progress"]),
          "native Continue Watching endpoint returns the current user's playback")
    status, cache = api_v1("GET", "/api/v1/torrents/" + file_hash + "/cache")
    check(status == 200 and cache["hash"] == file_hash and "global_cache" in cache,
          "native cache diagnostics expose per-torrent and global cache information")
    status, peers = api_v1("GET", "/api/v1/torrents/" + file_hash + "/peers")
    check(status == 200 and isinstance(peers["peers"], list),
          "native peer diagnostics return a list")
    status, reset = api_v1("DELETE", "/api/v1/torrents/" + file_hash + "/files/" + str(file_id) + "/progress")
    check(status == 200 and reset["status"] == "reset",
          "native API resets per-file viewed state and playback position")
    status, bad_stream = api_v1("GET", "/api/v1/torrents/" + file_hash + "/files/999/stream")
    check(status == 404, "native stream route validates file indexes and library ownership")

    status, before_settings = api_v1("GET", "/api/v1/settings")
    original_readahead = before_settings["settings"]["ReaderReadAHead"]
    original_disk_cache = before_settings["settings"]["DiskCacheEnabled"]
    original_disk_limit = before_settings["settings"]["DiskCacheSize"]
    original_list_cache = before_settings["settings"]["CacheListsInMemory"]
    status, storage_settings = api_v1("PUT", "/api/v1/settings", {"settings": {
        "DiskCacheEnabled": True, "DiskCacheSize": 3 * 1024 * 1024 * 1024,
        "CacheListsInMemory": False
    }})
    check(status == 200 and not storage_settings["restart_required"] and
          storage_settings["settings"]["DiskCacheEnabled"] and
          storage_settings["settings"]["DiskCacheSize"] == 3 * 1024 * 1024 * 1024 and
          not storage_settings["settings"]["CacheListsInMemory"],
          "disk cache and disk-backed catalog settings apply without restarting")
    status, disk_catalog = api_v1("GET", "/api/catalog/search?q=UI")
    check(status == 200, "catalog search works with the in-memory record cache disabled")
    status, restored_storage = api_v1("PUT", "/api/v1/settings", {"settings": {
        "DiskCacheEnabled": original_disk_cache, "DiskCacheSize": original_disk_limit,
        "CacheListsInMemory": original_list_cache
    }})
    check(status == 200 and restored_storage["settings"]["CacheListsInMemory"] == original_list_cache,
          "storage settings test restores the original cache modes")
    updated_readahead = 90 if original_readahead != 90 else 89
    status, saved_settings = api_v1("PUT", "/api/v1/settings",
                                   {"settings": {"ReaderReadAHead": updated_readahead}})
    check(status == 200 and saved_settings["settings"]["ReaderReadAHead"] == updated_readahead,
          "owner can update engine settings through the native API")
    status, _ = api_v1("PUT", "/api/v1/settings", {"settings": {"ReaderReadAHead": original_readahead}})
    check(status == 200, "engine settings test restores the original value")

    status, native_user = api_v1("POST", "/api/v1/users",
                                 {"username": "native-user", "password": "native-password", "rank": 10})
    check(status == 201 and native_user["username"] == "native-user",
          "native versioned user-management route creates a user")
    status, native_users = api_v1("GET", "/api/v1/users")
    check(status == 200 and any(x["id"] == native_user["id"] for x in native_users["users"]),
          "native user-management route lists accounts for an administrator")

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


def png_bytes(width, height):
    """A real PNG (RGB), no imaging library needed."""
    import struct
    import zlib

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + bytes([20 * row % 256, 60, 200]) * width for row in range(height))
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def make_video(path):
    """A short real video through ffmpeg; False when ffmpeg is not here."""
    try:
        subprocess.run(["ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                        "testsrc=size=320x240:rate=10:duration=3", "-pix_fmt", "yuv420p", path],
                       check=True, timeout=120)
    except (OSError, subprocess.SubprocessError):
        return False
    return os.path.exists(path) and os.path.getsize(path) > 0


def seed(directory, port):
    """Import libtorrent's python binding (as meshnet-test.py does) and seed a directory."""
    import libtorrent as lt

    files = lt.file_storage()

    def add(relative):
        full = os.path.join(directory, relative)
        for name in sorted(os.listdir(full)):
            child = relative + "/" + name
            if os.path.isdir(os.path.join(directory, child)):
                add(child)
            else:
                files.add_file(child, os.path.getsize(os.path.join(directory, child)))

    for name in sorted(os.listdir(directory)):
        if os.path.isdir(os.path.join(directory, name)):
            add(name)
    torrent = lt.create_torrent(files, 16384)
    lt.set_piece_hashes(torrent, directory)
    info = lt.torrent_info(lt.bencode(torrent.generate()))
    session = lt.session({"listen_interfaces": "127.0.0.1:%d" % port, "enable_dht": False,
                          "enable_lsd": False, "enable_upnp": False, "enable_natpmp": False})
    handle = session.add_torrent({"ti": info, "save_path": directory})
    handle.set_flags(lt.torrent_flags.seed_mode)
    deadline = time.time() + 60
    while not handle.is_seed() and time.time() < deadline:
        time.sleep(0.2)
    check(handle.is_seed(), "a local seeder holds the torrent for the cover test")
    return session, str(info.info_hash())


def cover_response(path, headers=None):
    request = urllib.request.Request(BASE + path, headers=headers or {})
    request.add_header("Authorization", "Bearer " + OWNER_TOKEN)
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            return response.status, dict(response.headers), response.read()
    except urllib.error.HTTPError as error:
        return error.code, dict(error.headers), error.read()


def wait_cover(cover_hash, seconds=180):
    deadline = time.time() + seconds
    while time.time() < deadline:
        status, headers, body = cover_response("/api/v1/torrents/" + cover_hash + "/cover")
        if status == 200:
            return headers, body
        if headers.get("X-P2Play-Cover") != "none":
            raise RuntimeError("the cover route answered " + str(status) + " without its marker")
        time.sleep(1)
    raise RuntimeError("no cover for " + cover_hash)


def covers(workdir):
    """A cover for a torrent added without a poster: the image inside it, or an ffmpeg frame."""
    try:
        import libtorrent  # noqa: F401
    except ImportError:
        print("SKIP the cover tests: the libtorrent binding is not installed")
        status, headers, _ = cover_response("/api/v1/torrents/" + "f" * 40 + "/cover")
        check(status == 404, "the cover route is not reachable for a torrent outside the library")
        return

    # the anonymous library entry of a torrent no seeder has: no cover, and that is remembered
    quiet = "f" * 40
    status, added = api_v1("POST", "/api/v1/torrents", {"link": "magnet:?xt=urn:btih:" + quiet})
    check(status == 201 and added["torrent"]["cover"] is False,
          "a torrent without a picture reports no cover in the library listing")
    status, headers, _ = cover_response("/api/v1/torrents/" + quiet + "/cover")
    check(status == 404 and headers.get("X-P2Play-Cover") == "none",
          "the cover route says there is no cover yet")

    # a poster the client sent is never replaced by the extractor
    explicit = "1" * 40
    status, named = api_v1("POST", "/api/v1/torrents", {"link": "magnet:?xt=urn:btih:" + explicit,
                                                       "poster": "http://example.invalid/p.jpg"})
    check(status == 201 and named["torrent"]["poster"] == "http://example.invalid/p.jpg" and
          named["torrent"]["cover"] is False,
          "a poster sent by the client keeps the torrent out of the cover extraction")

    root = os.path.join(workdir, "seed")
    os.makedirs(os.path.join(root, "Show"))
    picture = png_bytes(24, 16)
    with open(os.path.join(root, "Show", "poster.png"), "wb") as f:
        f.write(picture)
    video = os.path.join(root, "Show", "movie.mp4")
    has_ffmpeg = make_video(video)
    session, seeded = seed(root, 7811)
    try:
        status, added = api_v1("POST", "/api/v1/torrents", {
            "link": "magnet:?xt=urn:btih:" + seeded + "&dn=Show&x.pe=127.0.0.1:7811",
            "title": "Show"})
        check(status == 201 and added["torrent"]["hash"] == seeded,
              "the cover test torrent is in the library")
        headers, body = wait_cover(seeded)
        check(headers.get("Content-Type") == "image/png" and body == picture,
              "the cover is the poster image found inside the torrent")
        source = headers.get("X-P2Play-Cover", "")
        check(source.startswith("torrent:") and source.endswith("poster.png"),
              "the cover says which file of the torrent it came from: " + source)
        with open(os.path.join(workdir, "covers", seeded + ".img"), "rb") as f:
            check(f.read() == picture, "the cover is cached on disk by infohash")
        status, listed = api_v1("GET", "/api/v1/library/torrents?q=Show")
        entry = [x for x in listed["items"] if x["hash"] == seeded]
        check(status == 200 and entry and entry[0]["cover"] is True and
              entry[0]["cover_source"].endswith("poster.png"),
              "the library listing reports the cover and where it came from")
        etag = headers.get("ETag", "")
        check(etag != "", "the cover has an ETag")
        check(cover_response("/api/v1/torrents/" + seeded + "/cover",
                             {"If-None-Match": etag})[0] == 304,
              "a client that has the cover gets 304, not the bytes again")
        status, dropped = api_v1("DELETE", "/api/v1/torrents/" + seeded + "/cover")
        check(status == 200 and dropped["status"] == "dropped",
              "an owner can drop the cached cover")
        check(cover_response("/api/v1/torrents/" + seeded + "/cover")[0] == 404,
              "the dropped cover is gone")
        status, extracting = api_v1("POST", "/api/v1/torrents/" + seeded + "/cover", {})
        check(status == 202 and extracting["status"] == "extracting",
              "a refresh asks the server to extract the cover again")
        headers, body = wait_cover(seeded)
        check(body == picture, "the cover is extracted again after a refresh")
        status, refused = api_v1("POST", "/api/v1/torrents/" + explicit + "/cover", {})
        check(status == 409, "a torrent with a poster of its own is not given a server cover")
    finally:
        session.pause() if hasattr(session, "pause") else None

    # no image inside: a frame of the video, when ffmpeg is here
    if not has_ffmpeg:
        print("SKIP the ffmpeg cover test: no ffmpeg")
        return
    bare = os.path.join(workdir, "bare")
    os.makedirs(os.path.join(bare, "OnlyVideo"))
    check(make_video(os.path.join(bare, "OnlyVideo", "feature.mp4")), "a video for the probe")
    session, seeded_bare = seed(bare, 7812)
    status, added = api_v1("POST", "/api/v1/torrents", {
        "link": "magnet:?xt=urn:btih:" + seeded_bare + "&dn=OnlyVideo&x.pe=127.0.0.1:7812",
        "title": "Only video"})
    check(status == 201, "the video-only torrent is in the library")
    headers, body = wait_cover(seeded_bare)
    check(headers.get("Content-Type") == "image/jpeg" and headers.get("X-P2Play-Cover") == "probe",
          "with no image inside, a frame of the video is the cover (ffmpeg probe)")
    check(len(body) > 1000, "the probed frame is a JPEG of the video")


def wait_idle(page, seconds=30):
    """The page runs one request at a time (a busy flag); a click during it is dropped."""
    deadline = time.time() + seconds
    while time.time() < deadline:
        if page.get_attribute("body", "aria-busy") is None:
            return True
        time.sleep(0.1)
    return False


def wait_text(page, selector, needle, seconds=30):
    """The page's CSP forbids evaluating strings (no unsafe-eval), so poll the DOM from here."""
    deadline = time.time() + seconds
    while time.time() < deadline:
        if needle in page.inner_text(selector):
            return True
        time.sleep(0.2)
    return False


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
        wait_idle(page)
        page.click("[data-view=library]")
        page.wait_for_selector("#library-view:not([hidden])")
        page.fill("#library-link", "magnet:?xt=urn:btih:" + "e" * 40)
        page.fill("#library-title", "Browser Library Entry")
        page.fill("#library-new-category", "Browser")
        wait_idle(page)
        page.click("#library-add-form button")
        check(wait_text(page, "#library-rows", "Browser Library Entry"),
              "the new library entry appears in the list")
        check("Browser Library Entry" in page.inner_text("#library-rows"),
              "browser Library tab adds a magnet to the user library")
        check(page.locator("#library-rows .cover-placeholder").count() >= 1,
              "a torrent without a cover shows a placeholder, not a broken picture")
        page.locator("#library-rows .library-row").filter(has_text="Browser Library Entry").locator("button").filter(has_text="Details / files").click()
        page.wait_for_selector("#torrent-detail:not([hidden])")
        check(page.locator("#detail-cover-refresh").is_visible(), "the torrent detail offers a cover refresh")
        page.click("#detail-close")
        page.wait_for_selector("#torrent-detail", state="hidden")
        wait_idle(page)
        library_row = page.locator("#library-rows .library-row").filter(has_text="Browser Library Entry")
        library_row.locator("input[aria-label^='Title for']").fill("Edited Browser Entry")
        library_row.locator("input[aria-label^='Category for']").fill("Edited Category")
        wait_idle(page)
        library_row.locator("button").filter(has_text="Save changes").click()
        check(wait_text(page, "#library-rows", "Edited Browser Entry"),
              "the edited title appears in the list")
        edited_row = page.locator("#library-rows .library-row").filter(has_text="Edited Browser Entry")
        check(edited_row.locator("input[aria-label^='Category for']").input_value() == "Edited Category",
              "browser Library tab edits torrent title and category")
        page.locator("#library-rows .library-row").filter(has_text="Edited Browser Entry").locator("button").filter(has_text="Details / files").click()
        page.wait_for_selector("#torrent-detail:not([hidden])")
        page.wait_for_selector("#torrent-files")
        check("files" in page.inner_text("#torrent-detail").lower(),
              "browser displays torrent detail and file-management panel")
        wait_idle(page)
        page.click("[data-view=server]")
        page.wait_for_selector("#server-view:not([hidden])")
        # the storage and catalog cache controls loadServer() fills in; a missing input broke the
        # whole tab with a script error before
        check(page.evaluate("document.querySelector('#setting-disk-cache-enabled').checked") is not None,
              "the server tab shows the disk cache switch")
        check(page.evaluate("document.querySelector('#setting-cache-lists').checked") in (True, False),
              "the server tab shows the catalog cache switch")
        check(page.evaluate("document.querySelector('#setting-disk-cache-size').value").isdigit(),
              "the server tab shows the disk cache size")
        page.fill("#setting-readahead", "88")
        wait_idle(page)
        page.click("#server-form button")
        check(wait_text(page, "#status", "Settings saved"),
              "the server tab reports the saved settings")
        wait_idle(page)
        page.click("[data-view=users]")
        page.wait_for_selector("#users-view:not([hidden])")
        page.fill("#new-user-name", "browser-user")
        page.fill("#new-user-password", "browser-password")
        wait_idle(page)
        page.click("#user-add-form button")
        check(wait_text(page, "#users-rows", "browser-user"), "the new user appears in the list")
        wait_idle(page)
        page.click("[data-view=continue]")
        page.wait_for_selector("#continue-view:not([hidden])")
        wait_idle(page)
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
        covers(root)
        if "--browser" in sys.argv:
            browser()
        print("PASS")
    finally:
        stop_daemon(proc)


if __name__ == "__main__":
    main()
