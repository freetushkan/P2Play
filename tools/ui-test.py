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
HTTP_PREFIX = ""


def request_url(path, prefixed=True):
    return BASE + (HTTP_PREFIX if prefixed else "") + path
OWNER_PASSWORD = "test"
OWNER_TOKEN = "ts_test00000000000000000000"


def check(cond, what):
    print(("OK   " if cond else "FAIL ") + what)
    if not cond:
        raise RuntimeError(what)


def start_daemon(daemon, data, base_path=""):
    if not os.path.exists(os.path.join(data, "auth.json")):
        with open(os.path.join(data, "auth.json"), "w") as f:
            json.dump({"users": [{"username": "owner", "password": OWNER_PASSWORD,
                                  "api_token": OWNER_TOKEN}]}, f)
    log = open(os.path.join(data, "stdout.log"), "a")
    args = [daemon, "--data", data, "--port", "8191"]
    if base_path:
        args.extend(["--base-path", base_path])
    proc = subprocess.Popen(args,
                            stdout=log, stderr=subprocess.STDOUT)
    log.close()
    for _ in range(100):
        try:
            if get("/api/v1/system/ping")[0] == 200:
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


def get(path, headers=None, prefixed=True):
    req = urllib.request.Request(request_url(path, prefixed), headers=headers or {})
    try:
        with urllib.request.build_opener(NoRedirect).open(req, timeout=10) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


def call(path, body=None):
    req = urllib.request.Request(request_url(path), data=None if body is None else json.dumps(body).encode(),
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
    req = urllib.request.Request(request_url(path), data=payload, method=method)
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


def make_torrs_link(infohash, title, poster="", category="", size=0, trackers=()):
    """Build the public torrs:// card representation (base62 of zlib metadata)."""
    import struct
    import zlib

    payload = bytearray(bytes.fromhex(infohash))

    def field(tag, value):
        raw = value.encode("utf-8")
        payload.append(tag)
        payload.extend(struct.pack("<H", len(raw)))
        payload.extend(raw)

    field(0, title)
    if poster:
        field(1, poster)
    if category:
        field(3, category)
    if size:
        payload.append(4)
        payload.extend(struct.pack("<Q", size))
    for tracker in trackers:
        field(2, tracker)

    compressed = zlib.compress(bytes(payload))
    alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
    number = int.from_bytes(compressed, "big")
    digits = ""
    while number:
        number, remainder = divmod(number, 62)
        digits = alphabet[remainder] + digits
    leading_zeroes = 0
    for byte in compressed:
        if byte:
            break
        leading_zeroes += 1
    return "torrs://" + ("0" * leading_zeroes) + digits


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

    for scheme in ("http", "https"):
        status, rejected = api_v1("POST", "/api/v1/torrents", {
            "link": scheme + "://127.0.0.1:9/private.torrent"
        })
        check(status == 400 and "publicly reachable" in rejected.get("error", ""),
              scheme.upper() + " torrent URLs reject loopback targets")

    second_hash = "b" * 40
    preview_hash = "d" * 40
    status, preview = api_v1("POST", "/api/v1/torrents/preview", {
        "link": "magnet:?xt=urn:btih:" + preview_hash, "title": "Preview only"
    })
    check(status == 200 and preview["preview"] and preview["torrent"]["hash"] == preview_hash,
          "native preview route returns an ephemeral torrent")
    status, preview_library = api_v1("GET", "/api/v1/library/torrents?q=Preview%20only")
    check(status == 200 and all(item["hash"] != preview_hash for item in preview_library["items"]),
          "preview torrents do not enter the persistent library")

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
    status, file_list = api_v1("GET", "/api/v1/torrents/" + file_hash + "/files");
    status, file_share = api_v1("POST", "/api/v1/torrents/" + file_hash + "/files/0/share", {})
    check(status == 200 and "/files/0/stream?token=" in file_share["url"] and file_share["file"],
          "file share URL contains the stable user token")
    status, torrent_share = api_v1("POST", "/api/v1/torrents/" + file_hash + "/share", {})
    check(status == 200 and "/stream?token=" in torrent_share["url"] and torrent_share["file"],
          "torrent share URL selects a playable file")
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
    status, disk_catalog = api_v1("GET", "/api/v1/catalog/search?q=UI")
    check(status == 200, "catalog search works with the in-memory record cache disabled")
    status, restored_storage = api_v1("PUT", "/api/v1/settings", {"settings": {
        "DiskCacheEnabled": original_disk_cache, "DiskCacheSize": original_disk_limit,
        "CacheListsInMemory": original_list_cache
    }})
    check(status == 200 and restored_storage["settings"]["CacheListsInMemory"] == original_list_cache,
          "storage settings test restores the original cache modes")
    original_mesh_directory = before_settings["settings"]["DisableMeshDirectory"]
    status, mesh_disabled = api_v1("PUT", "/api/v1/settings",
                                   {"settings": {"DisableMeshDirectory": not original_mesh_directory}})
    check(status == 200 and mesh_disabled["restart_required"],
          "changing public MediaMesh directory advertisement requires a restart")
    status, mesh_restored = api_v1("PUT", "/api/v1/settings",
                                   {"settings": {"DisableMeshDirectory": original_mesh_directory}})
    check(status == 200 and mesh_restored["restart_required"] and
          mesh_restored["settings"]["DisableMeshDirectory"] == original_mesh_directory,
          "the original public MediaMesh directory setting is restored")
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

    status, prefs = api_v1("GET", "/api/v1/preferences")
    check(status == 200 and prefs["preferences"] == {"language": "en", "theme": "light"},
          "native UI preferences default to English and light theme")
    status, prefs = api_v1("PUT", "/api/v1/preferences", {"language": "ru", "theme": "dark"})
    check(status == 200 and prefs["preferences"] == {"language": "ru", "theme": "dark"},
          "native UI preferences accept Russian and dark theme")
    status, auto_prefs = api_v1("PUT", "/api/v1/preferences", {"theme": "auto"})
    check(status == 200 and auto_prefs["preferences"]["theme"] == "auto",
          "native UI preferences persist automatic theme")
    status, dark_prefs = api_v1("PUT", "/api/v1/preferences", {"theme": "dark"})
    check(status == 200 and dark_prefs["preferences"]["theme"] == "dark",
          "native UI preferences can restore a fixed theme")
    status, other_prefs = api_v1("GET", "/api/v1/preferences", token=native_user["api_token"])
    check(status == 200 and other_prefs["preferences"] == {"language": "en", "theme": "light"},
          "UI preferences are isolated per user")
    status, invalid_prefs = api_v1("PUT", "/api/v1/preferences", {"language": "xx"})
    check(status == 400, "native UI preferences reject unsupported locales")
    status, restored_prefs = api_v1("PUT", "/api/v1/preferences", {"language": "en", "theme": "light"})
    check(status == 200 and restored_prefs["preferences"] == {"language": "en", "theme": "light"},
          "native UI preferences can be restored to defaults")

    status, categories = api_v1("GET", "/api/v1/library/categories")
    check(status == 200 and set(categories["categories"]) == {"Animation", "Files", "Series"},
          "library category endpoint returns the current user's distinct categories")

    owner_user = call("/api/v1/users", {"username": "library-user", "password": "test-user", "rank": 10})
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

    torrs_hash = "c" * 40
    remote_poster = "http://127.0.0.1:9/no-poster.png"
    torrs = make_torrs_link(torrs_hash, "Torrs Card Title", remote_poster, "Films", 123456,
                            ["udp://127.0.0.1:1234/announce"])
    status, torrs_added = api_v1("POST", "/api/v1/torrents", {"link": torrs})
    check(status == 201 and torrs_added["torrent"]["hash"] == torrs_hash and
          torrs_added["torrent"]["title"] == "Torrs Card Title",
          "native API parses a torrs:// card and imports its torrent metadata")
    check(torrs_added["torrent"]["poster"] == remote_poster,
          "an unsafe/unreachable torrs:// poster does not block the add or escape as a fetched image")
    status, _ = api_v1("DELETE", "/api/v1/torrents/" + torrs_hash)
    check(status == 200, "torrs:// regression torrent is removed from the test library")

    # A portable library file is catalogue data, not a request to activate every swarm.
    bulk_cards = [make_torrs_link(format(i, "040x"), "Bulk card %04d " % i + os.urandom(2048).hex(),
                                 category="Bulk") for i in range(64)]
    bulk_text = ("\n".join(bulk_cards) + "\n").encode()
    check(len(bulk_text) > 100 * 1024, "bulk .torrs fixture is substantially larger than a small list")
    stats_status, stats_before = api_v1("GET", "/api/v1/system/stats")
    import_status, imported = api_v1("POST", "/api/v1/library/import", bulk_text,
                                     content_type="text/plain; charset=utf-8")
    stats_after_status, stats_after = api_v1("GET", "/api/v1/system/stats")
    check(import_status == 200 and imported["added"] == len(bulk_cards) and
          imported["rejected"] == 0 and imported["skipped"] == 0,
          "large .torrs import adds all catalogue cards")
    check(stats_status == 200 and stats_after_status == 200 and
          stats_after["stats"]["active_sessions"] == stats_before["stats"]["active_sessions"],
          "bulk .torrs import does not activate or download all imported swarms")
    for i in range(len(bulk_cards)):
        status, _ = api_v1("DELETE", "/api/v1/torrents/" + format(i, "040x"))
        check(status == 200, "bulk import test card can be removed")



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


def make_video_with_attached_cover(video, picture):
    """Put a real attached_pic video stream into MP4; False if ffmpeg cannot mux it."""
    base, _ = os.path.splitext(video)
    poster_png, poster_jpg, output = base + ".cover.png", base + ".cover.jpg", base + ".attached.mp4"
    with open(poster_png, "wb") as f:
        f.write(picture)
    try:
        subprocess.run(["ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i", poster_png,
                        "-frames:v", "1", poster_jpg], check=True, timeout=30)
        subprocess.run(["ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i", video, "-i", poster_jpg,
                        "-map", "0:v:0", "-map", "1:v:0", "-c:v:0", "copy", "-c:v:1", "mjpeg",
                        "-disposition:v:1", "attached_pic", output], check=True, timeout=60)
        os.replace(output, video)
    except (OSError, subprocess.SubprocessError):
        return False
    finally:
        for path in (poster_png, poster_jpg, output):
            try:
                os.remove(path)
            except OSError:
                pass
    return os.path.exists(video) and os.path.getsize(video) > 0


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
    request = urllib.request.Request(request_url(path), headers=headers or {})
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


def multipart_parts(fields, files):
    boundary = "p2play-poster-test-boundary"
    chunks = []
    for name, value in fields.items():
        chunks.extend([
            ("--" + boundary + "\r\n").encode(),
            ('Content-Disposition: form-data; name="' + name + '"\r\n\r\n').encode(),
            value.encode(), b"\r\n"
        ])
    for name, file_name, data, mime in files:
        chunks.extend([
            ("--" + boundary + "\r\n").encode(),
            ('Content-Disposition: form-data; name="' + name + '"; filename="' + file_name + '"\r\n').encode(),
            ("Content-Type: " + mime + "\r\n\r\n").encode(), data, b"\r\n"
        ])
    chunks.append(("--" + boundary + "--\r\n").encode())
    return b"".join(chunks), "multipart/form-data; boundary=" + boundary


def poster_bytes(path, token=OWNER_TOKEN):
    req = urllib.request.Request(request_url(path), headers={"Authorization": "Bearer " + token})
    try:
        with urllib.request.urlopen(req, timeout=15) as response:
            return response.status, dict(response.headers), response.read()
    except urllib.error.HTTPError as error:
        return error.code, dict(error.headers), error.read()


def posters():
    """User-uploaded posters are stored on the node and served by its own authenticated URL."""
    torrent_hash = "c" * 40
    picture = png_bytes(12, 8)
    body, content_type = multipart_parts(
        {"link": "magnet:?xt=urn:btih:" + torrent_hash, "title": "Custom poster test"},
        [("poster_file", "custom.png", picture, "image/png")]
    )
    status, added = api_v1("POST", "/api/v1/torrents", body, content_type=content_type)
    poster_url = "/api/v1/torrents/" + torrent_hash + "/poster"
    check(status == 201 and added["torrent"]["poster"] == poster_url and
          added["torrent"]["cover"] is False,
          "multipart add stores the custom poster URL in the torrent JSON")
    status, headers, image = poster_bytes(poster_url)
    check(status == 200 and headers.get("Content-Type") == "image/png" and
          headers.get("X-P2Play-Poster") == "client" and image == picture,
          "the P2Play poster route returns the exact uploaded bytes")
    status, detail = api_v1("GET", "/api/v1/torrents/" + torrent_hash)
    check(status == 200 and detail["torrent"]["poster"] == poster_url and
          detail["torrent"]["cover"] is False,
          "a user poster takes priority over automatic cover extraction")
    status, denied = api_v1("POST", poster_url, {"url": "http://127.0.0.1/private.png"})
    check(status == 400, "poster URL fetching refuses loopback and private targets")
    status, removed = api_v1("DELETE", poster_url)
    check(status == 200 and removed["status"] == "removed",
          "a custom poster can be removed from the torrent")
    status, _, _ = poster_bytes(poster_url)
    check(status == 404, "the custom poster bytes are removed with the poster")
    status, _ = api_v1("DELETE", "/api/v1/torrents/" + torrent_hash)
    check(status == 200, "the custom-poster test torrent can be removed")


def covers(workdir):
    """A cover for a torrent added without a poster: file image, embedded artwork, optional frame."""
    status, settings = api_v1("GET", "/api/v1/settings")
    cover_settings = settings["settings"]
    check(status == 200 and cover_settings.get("CoverFiles") is True and
          cover_settings.get("CoverEmbedded") is True and cover_settings.get("CoverProbe") is False,
          "image-file and embedded-art sources default on, regular video-frame extraction defaults off")
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

    # Embedded attached_pic takes priority over an ordinary video frame. CoverProbe is still off here.
    import shutil
    if has_ffmpeg and shutil.which("ffprobe"):
        embedded_dir = os.path.join(workdir, "embedded")
        os.makedirs(os.path.join(embedded_dir, "EmbeddedArt"))
        embedded_video = os.path.join(embedded_dir, "EmbeddedArt", "movie.mp4")
        embedded_ok = make_video(embedded_video) and make_video_with_attached_cover(embedded_video, picture)
        check(embedded_ok, "a video with an attached cover stream can be muxed")
        if embedded_ok:
            embedded_session, embedded_hash = seed(embedded_dir, 7813)
            try:
                status, added = api_v1("POST", "/api/v1/torrents", {
                    "link": "magnet:?xt=urn:btih:" + embedded_hash + "&dn=EmbeddedArt&x.pe=127.0.0.1:7813",
                    "title": "Embedded artwork"})
                check(status == 201, "the embedded-art torrent is in the library")
                headers, body = wait_cover(embedded_hash)
                check(headers.get("Content-Type") == "image/jpeg" and
                      headers.get("X-P2Play-Cover", "").startswith("embedded:"),
                      "the server extracts attached_pic artwork before any frame fallback")
            finally:
                embedded_session.pause() if hasattr(embedded_session, "pause") else None
                api_v1("DELETE", "/api/v1/torrents/" + embedded_hash)
    else:
        print("SKIP the embedded-attached-picture test: ffmpeg or ffprobe is not here")

    # A regular frame is a separate, explicit fallback; it must not be enabled by default.
    if not has_ffmpeg:
        print("SKIP the ffmpeg cover test: no ffmpeg")
        return
    status, frame_enabled = api_v1("PUT", "/api/v1/settings", {"settings": {"CoverProbe": True}})
    check(status == 200 and frame_enabled["settings"]["CoverProbe"] is True,
          "the owner can explicitly enable frame extraction")
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


def ui_assets():
    with open(os.path.join(PLUGIN, "index.html"), encoding="utf-8") as source:
        page = source.read()
    with open(os.path.join(PLUGIN, "app.js"), encoding="utf-8") as source:
        app = source.read()
    with open(os.path.join(PLUGIN, "style.css"), encoding="utf-8") as source:
        css = source.read()
    with open(os.path.join(PLUGIN, "lang", "ru.json"), encoding="utf-8") as source:
        ru = json.load(source)
    check('data-settings-view="preferences" class="active">General</button>' in page,
          "settings starts at the General section")
    check('id="theme-select"' not in page and 'id="language-select-settings"' not in page and
          "theme-select" not in app and "language-select-settings" not in app,
          "theme and language controls are not duplicated in Settings")
    check("statusTimer" in app and "clearTimeout" in app and "setTimeout" in app and
          "#status.visible" in css and 'aria-hidden="true"' in page,
          "status notifications auto-dismiss and remain hidden otherwise")
    check('status("Lists loaded.")' not in app and
          'status("Search subscribed catalogs.")' not in app and
          'status("Loading library…")' not in app and
          'status(filtered+' not in app and 'status(list.length+' not in app,
          "routine navigation and loading do not create notifications")
    check('tr("Files: ")' in app and 'tr("files")' not in app and "Files: " in ru,
          "file counts keep the number after a localized label")
    check("No lists subscribed." in ru and "No public lists available." in ru and
          ru.get("General") == "Основные",
          "list empty states and General are translated")
    check('tr(discovery?"No public lists available.":"No lists subscribed.")' in app and
          'tr(discovery?"Subscribe":"Request refresh")' in app and
          'l.description||tr("No description")' in app and 'tr("Version ")+l.seq' in app,
          "list descriptions, versions and subscription actions are localized")
    check('class="account-avatar button-icon"' in page and 'class="button-icon"' in page,
          "user and search actions have vector icons")


def browser():
    from playwright.sync_api import sync_playwright
    pk = call("/api/v1/publish", {"name": "UI list", "description": "for the page"})["pubkey"]
    ih = "d" * 40
    req = urllib.request.Request(request_url(f"/api/v1/publish/{pk}/records"), method="POST",
                                 data=json.dumps({"ih": ih, "title": "Мультфильм Ёжик (1975)", "tags": ["cartoon"]}, ensure_ascii=False).encode())
    req.add_header("Authorization", "Bearer " + OWNER_TOKEN)
    urllib.request.urlopen(req, timeout=30).read()
    call(f"/api/v1/publish/{pk}/release", {})
    with sync_playwright() as p:
        b = p.chromium.launch()
        page = b.new_page()
        errors = []
        page.on("pageerror", lambda e: errors.append(str(e)))
        page.goto(request_url("/"))
        check(page.url.endswith("/ui/"), "the browser at /ui/")
        page.wait_for_selector("#login-view:not([hidden])")
        page.fill("#password", OWNER_PASSWORD)
        page.click("#login-form button")
        page.wait_for_selector("#library-view:not([hidden])")
        check(not page.locator("#library-tab").is_hidden(), "core-host exposes the Library tab")
        check(page.title() == "P2Play", "logged in on the page")
        check("Library" in page.inner_text("#library-view"), "native UI opens the library by default")
        check(page.locator("#account-menu-wrap").is_visible() and
              page.locator("#identity-role").inner_text() == "Owner",
              "the account menu shows the username with its translated role")
        page.click("#account-menu-trigger")
        check(page.locator("#logout").is_visible(), "account menu exposes the logout action")
        page.click("#account-menu-trigger")
        page.select_option("#language-select", "ru")
        check(wait_text(page, "#library-heading", "Библиотека"), "Russian UI locale is applied")
        check(wait_text(page, "#identity-role", "Владелец"), "the account role is translated in Russian")
        wait_idle(page)
        page.select_option("#language-select", "en")
        check(wait_text(page, "#library-heading", "Library"), "English UI locale can be restored")
        wait_idle(page)
        page.click("#theme-toggle")
        wait_idle(page)
        check(page.locator("html").get_attribute("data-theme") == "dark", "dark theme can be enabled")
        page.click("#theme-toggle")
        wait_idle(page)
        check(page.locator("html").get_attribute("data-theme") == "auto", "automatic theme can be selected")
        page.click("#theme-toggle")
        wait_idle(page)
        check(page.locator("html").get_attribute("data-theme") == "light", "light theme can be restored")
        page.click("#search-tab")
        page.wait_for_selector("#search-view:not([hidden])")
        page.fill("#query", "ежик")
        page.click("#search-form button")
        page.wait_for_selector("#results .row")
        check("Ёжик" in page.inner_text("#results"), "found in Cyrillic, ё as е")
        page.click("#results .row button")
        page.wait_for_selector("#results .row button:disabled")
        check(any(t["hash"] == ih for t in call("/api/v1/torrents")["torrents"]), "added: in the owner's library")
        wait_idle(page)
        page.click("[data-view=library]")
        page.wait_for_selector("#library-view:not([hidden])")
        page.click("#add-open")
        page.wait_for_selector("#add-modal:not([hidden])")
        page.click("#add-search-open")
        page.fill("#add-query", "ежик")
        page.click("#add-search-form button[type=submit]")
        check(wait_text(page, "#add-search-results", "Ёжик"), "catalog search is available inside the add dialog")
        page.click("#add-search-results .row button")
        check(page.locator("#add-modal").is_visible() and page.locator("#add-form-panel").is_visible(),
              "selecting a catalog result returns to the parent add dialog")
        page.fill("#library-link", "magnet:?xt=urn:btih:" + "e" * 40)
        page.fill("#library-title", "Browser Library Entry")
        page.fill("#library-new-category", "Browser")
        page.set_input_files("#library-poster-file", {
            "name": "browser-poster.png", "mimeType": "image/png", "buffer": png_bytes(24, 16)
        })
        wait_idle(page)
        page.click("#library-add-form button[type=submit]")
        page.wait_for_selector("#add-modal", state="hidden")
        check(wait_text(page, "#library-rows", "Browser Library Entry"),
              "the new library entry appears in the list")
        check(page.locator("#library-filter-count").inner_text() == "(2/2)",
              "the filter disclosure shows filtered and total counts")
        check(page.locator("#library-pager").is_hidden(),
              "single-page libraries do not show pagination controls")
        check("Browser Library Entry" in page.inner_text("#library-rows"),
              "browser Library tab adds a magnet to the user library")
        custom_row = page.locator("#library-rows .library-row").filter(has_text="Browser Library Entry")
        check(custom_row.locator("img.cover-thumb").count() == 1 and
              custom_row.locator("img.cover-thumb").get_attribute("src").endswith("/poster"),
              "a user-selected poster is the library card image")
        page.locator("#library-rows .library-row").filter(has_text="Browser Library Entry").locator("button").filter(has_text="Details / files").click()
        page.wait_for_selector("#torrent-detail:not([hidden])")
        check(page.locator("#detail-cover-refresh").is_visible(), "the torrent detail offers a cover refresh")
        page.click("#detail-close")
        page.wait_for_selector("#torrent-detail", state="hidden")
        wait_idle(page)
        library_row = page.locator("#library-rows .library-row").filter(has_text="Browser Library Entry")
        library_row.locator("h3.torrent-title").click()
        title_editor = library_row.locator("input.title-editor")
        title_editor.fill("Edited Browser Entry")
        with page.expect_response(lambda response: response.request.method == "PUT" and "/api/v1/torrents/" in response.url):
            title_editor.press("Enter")
        edited_row = page.locator("#library-rows .library-row").filter(has_text="Edited Browser Entry")
        category_editor = edited_row.locator("input.category-editor")
        category_editor.fill("Edited Category")
        with page.expect_response(lambda response: response.request.method == "PUT" and "/api/v1/torrents/" in response.url):
            category_editor.press("Tab")
        check(wait_text(page, "#library-rows", "Edited Browser Entry"),
              "the title is saved with Enter from inline editing")
        edited_row = page.locator("#library-rows .library-row").filter(has_text="Edited Browser Entry")
        check(edited_row.locator("button").count() == 2 and
              edited_row.locator("button").nth(0).inner_text() == "Remove" and
              edited_row.locator("button").nth(1).inner_text() == "Details / files",
              "the compact torrent card keeps only Remove and Details actions")
        check(edited_row.locator("input.category-editor").input_value() == "Edited Category",
              "category is saved on blur or Enter without a save button")
        page.locator("#library-rows .library-row").filter(has_text="Edited Browser Entry").locator("button").filter(has_text="Details / files").click()
        page.wait_for_selector("#torrent-detail:not([hidden])")
        page.wait_for_selector("#torrent-files")
        check("files" in page.inner_text("#torrent-detail").lower(),
              "browser displays torrent detail and file-management panel")
        wait_idle(page)
        page.click("#settings-open")
        page.wait_for_selector("#settings-modal:not([hidden])")
        page.click("[data-settings-view=server]")
        page.wait_for_selector("#server-view:not([hidden])")
        # the storage and catalog cache controls loadServer() fills in; a missing input broke the
        # whole tab with a script error before
        check(page.evaluate("document.querySelector('#setting-disk-cache-enabled').checked") is not None,
              "the server tab shows the disk cache switch")
        check(page.evaluate("document.querySelector('#setting-cache-lists').checked") in (True, False),
              "the server tab shows the catalog cache switch")
        check(page.locator("#disk-cache-size-control").is_hidden(),
              "the disk cache size slider is hidden when disk cache is disabled")
        page.check("#setting-disk-cache-enabled")
        check(page.locator("#disk-cache-size-control").is_visible(),
              "enabling disk cache reveals its size slider")
        page.fill("#setting-readahead", "88")
        wait_idle(page)
        page.click("#server-form button")
        check(wait_text(page, "#status", "Settings saved"),
              "the server tab reports the saved settings")
        wait_idle(page)
        page.click("[data-settings-view=users]")
        page.wait_for_selector("#users-view:not([hidden])")
        page.fill("#new-user-name", "browser-user")
        page.fill("#new-user-password", "browser-password")
        wait_idle(page)
        page.click("#user-add-form button")
        check(wait_text(page, "#users-rows", "browser-user"), "the new user appears in the list")
        wait_idle(page)
        new_user = page.locator("#users-rows .user-row").filter(has_text="browser-user")
        new_user.locator("button").filter(has_text="Reset password").click()
        page.wait_for_selector("#dialog-modal:not([hidden])")
        page.fill("#dialog-input", "browser-password-updated")
        page.click("#dialog-accept")
        page.wait_for_selector("#dialog-modal", state="hidden")
        wait_idle(page)
        page.click("#settings-close")
        page.wait_for_selector("#settings-modal", state="hidden")
        page.click("#continue-tab")
        page.wait_for_selector("#continue-view:not([hidden])")
        wait_idle(page)
        page.click("#settings-open")
        page.wait_for_selector("#settings-modal:not([hidden])")
        page.click("[data-settings-view=lists]")
        page.wait_for_selector("#lists-view:not([hidden])")
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
        for path, name in (("/ui/", "index.html"), ("/ui/index.html", "index.html"), ("/ui/app.js", "app.js"),
                           ("/ui/style.css", "style.css"), ("/ui/lang/en.json", "lang/en.json"),
                           ("/ui/lang/ru.json", "lang/ru.json")):
            status, headers, body = get(path)
            with open(os.path.join(PLUGIN, name), "rb") as f:
                same = f.read() == body
            check(status == 200 and same and "frame-ancestors" in headers.get("Content-Security-Policy", "")
                  and headers.get("X-Content-Type-Options") == "nosniff", path + ": the plugin's " + name)
        check(get("/ui/service.js")[0] == 404 and get("/ui/info.yaml")[0] == 404, "the plugin's server side not served")
        ui_assets()
        check(get("/api/v1/system/version")[0] == 200 and
              json.loads(get("/api/v1/system/version")[2])["name"] == "P2Play",
              "native public P2Play identity is available without MatriX")
        matrix_status, matrix_headers, _ = get("/torrents", {"Authorization": "Basic dGVzdDp0ZXN0"})
        check(matrix_status == 404 and "WWW-Authenticate" not in matrix_headers,
              "MatriX routes and Basic challenge are removed")
        check(get("/echo")[0] == 404, "MatriX discovery route is removed")
        check(get("/api/v1/auth/me")[0] == 401, "the API behind the page asks a login")
        native_library()
        posters()
        covers(root)
        if "--browser" in sys.argv:
            browser()
        print("PASS")
    finally:
        stop_daemon(proc)
    prefixed_server_test(daemon)


def prefixed_server_test(daemon):
    global HTTP_PREFIX
    HTTP_PREFIX = "/some_sub_dir/more_path/ololo"
    with tempfile.TemporaryDirectory(prefix="p2play-prefix-") as root:
        proc = start_daemon(daemon, root, HTTP_PREFIX)
        try:
            status, _, body = get("/api/v1/system/ping")
            check(status == 200 and json.loads(body)["status"] == "ok",
                  "multi-level base path routes the native API")
            check(get("/api/v1/system/ping", prefixed=False)[0] == 404,
                  "native API is not exposed outside the configured base path")
            check(get("/some_sub_dir/api/v1/system/ping", prefixed=False)[0] == 404,
                  "partial base path does not expose nested API routes")
            check(get("/ui/")[0] == 200,
                  "web UI is served below the configured base path")
            check(get("/ui/", prefixed=False)[0] == 404,
                  "web UI is not exposed at the unprefixed path")
            status, headers, _ = get("/ui")
            check(status == 301 and headers.get("Location") == HTTP_PREFIX + "/ui/",
                  "UI redirect preserves the configured base path")
            status, body = api_v1("POST", "/api/v1/auth/login",
                                  {"username": "owner", "password": OWNER_PASSWORD})
            check(status == 200, "login works below the configured base path")
            req = urllib.request.Request(request_url("/api/v1/auth/login"),
                                         data=json.dumps({"username": "owner", "password": OWNER_PASSWORD}).encode(),
                                         method="POST", headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(req, timeout=10) as response:
                cookie = response.headers.get("Set-Cookie", "")
            check("Path=" + HTTP_PREFIX in cookie,
                  "authentication cookie is scoped to the configured base path")

            piece = hashlib.sha1(b"x").digest()
            info = (b"d6:lengthi1e4:name7:one.bin" +
                    b"12:piece lengthi16384e6:pieces20:" + piece + b"e")
            torrent_file = b"d4:info" + info + b"e"
            body, content_type = multipart({"title": "Prefixed share"}, "one.torrent", torrent_file)
            status, uploaded = api_v1("POST", "/api/v1/torrents", body,
                                      content_type=content_type)
            file_hash = uploaded["torrent"]["hash"] if uploaded and uploaded.get("torrent") else ""
            check(status == 201 and len(file_hash) == 40,
                  "multipart torrent upload works below the configured base path")
            status, shared = api_v1("POST", "/api/v1/torrents/" + file_hash + "/files/0/share", {})
            check(status == 200 and shared["url"].startswith(
                HTTP_PREFIX + "/api/v1/torrents/" + file_hash + "/files/0/stream?token="),
                "generated stream share URL includes the full configured base path")
            status, _ = api_v1("DELETE", "/api/v1/torrents/" + file_hash)
            check(status == 200, "prefixed base-path test removes its temporary torrent")
        finally:
            stop_daemon(proc)
    HTTP_PREFIX = ""


if __name__ == "__main__":
    main()
