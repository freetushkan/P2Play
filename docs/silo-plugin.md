# MediaMesh for Silo

`plugins/mediamesh/` is a real server-side JavaScript/goja Silo plugin, not an
iframe, a Go-core patch, or a browser client holding a daemon API token. It adds
search, list discovery/subscription, and transfer of a selected catalog release
into the signed-in user's native Silo torrent library.

## One interface, two hosts

`index.html`, `app.js` and `style.css` are also the P2Play core's own web interface:
`cmake/embed-ui.cmake` builds them into the core, which serves them at `/ui/` (a browser
at `/` is sent there). There is no second copy. `app.js` tells the host by the address:
under `/plugins/` it calls this plugin's routes (`service.js`); anywhere else it calls the
core's `/api` in the same shapes (search, lists, discovery, subscription, adding to the
signed-in user's library) and logs in through `/api/auth/login`. Elements only Silo has
(the back link, the daemon settings, Silo's permission notes) carry `silo-only`.
`index.html` refers to its files relatively, so the page works at `/plugins/mediamesh/`
and at `/ui/`. `tools/ui-test.py` checks the core side (with `--browser`: the page itself,
in Chromium).

## Compatibility pin

Inspected and tested against **YouROK/TorrServer, branch `silo`, commit
`3610db846fd2132f06aa626a4cb94f4a0cfa69ae`**. The upstream branch is explicitly
labelled early development; do not assume compatibility with `master`, a stable
TorrServer release, or a future Silo commit.

Source: <https://github.com/YouROK/TorrServer/tree/3610db846fd2132f06aa626a4cb94f4a0cfa69ae>.
Initial inspection used the GitHub web page, followed by a separate isolated clone.
The integration does not vendor or modify upstream Go code.

Verified upstream contracts (paths relative to that commit):

- `internal/plugin/manifest.go`, `manager.go`: `info.yaml`, `entry`, route whitelist,
  ZIP installation, asynchronous goja execution, namespaced plugin storage.
- `internal/plugin/bind_web.go`: `ts.web.get/post/staticFile`, `req.query`, lower-case
  `req.headers`, parsed JSON `req.body`, `req.raw`, `req.user`, and chained
  `res.status/header/json/html`. Plugin routes are **not** automatically inside
  core `AuthMiddleware`; this plugin checks `req.user` on every dynamic route.
- `internal/plugin/bind_http.go`: synchronous `ts.http.get(url, options)` and
  `ts.http.post(url, body, options)`, `options.headers`, integer timeout in ms,
  `response.ok/status/body/json()`. No `request`, `delete`, `fetch`, Promises, Node
  APIs, browser `URL`, or user-controlled generic proxy endpoint is assumed.
- `internal/plugin/bind_storage.go`, `bind_crypto.go`, `bind_vfs.go`:
  `ts.storage.get/set` store strings in BoltDB; `ts.crypto.randomBytes` and
  `hmacSha256` issue per-user CSRF values, verified with `constantTimeCompare`; `ts.vfs.read` serves the HTML.
- `internal/plugin/bind_torrent.go`: `ts.torrent.add(magnet, {title, category,
  user_id, save_to_db})`. The explicit `user_id` is essential: omitted options
  default to **owner**, not the authenticated visitor.
- `internal/torrent/helpers.go`: `ParseTorrentSpec` accepts a magnet or a
  **40-character v1 infohash only**. It does **not** accept `torrs://` or v2 hashes.
- `internal/plugin/bind_web.go`, `internal/web/middleware.go`, `api_auth.go`,
  `internal/user/model.go`: Silo authentication and banned-user checks, cookie
  `silo_token`, SameSite=Lax, user rank 10 and administrator rank 50.

## Install

1. Set a non-empty Silo `auth.owner_password` **before installing**. In upstream
   open mode every visitor becomes owner, regardless of the plugin's rank checks.
   Keep Silo behind authenticated HTTPS access; its current cookie is not marked
   Secure. Do not expose a password-free Silo instance to a network.
2. From the P2Play repository root, package the UI plugin files with `info.yaml`
   at ZIP root (Python standard library, no dependencies):

   ```sh
   python3 -I -c "from pathlib import Path; from zipfile import ZipFile; p=Path('plugins/mediamesh'); z=ZipFile('mediamesh.zip','w'); [z.write(p/n,n) for n in ['info.yaml','service.js','index.html','app.js','style.css','lang/en.json','lang/ru.json']]; z.close()"
   ```

   On Windows, use `python` instead of `python3` if necessary. The archive contains
   no persisted configuration or credentials. Do not ZIP Silo's data directory.
3. As a Silo administrator, upload the ZIP through the existing plugin manager
   (or its authenticated `POST /api/plugins/upload`). No core rebuild is needed.
   Upstream installs/loads the archive itself; manually copying a directory does
   not necessarily create the required enabled plugin state.
4. Open `/plugins/mediamesh/` after signing in. The sidebar entry is visible at
   rank 10+. Settings is visible and callable only at rank 50+.
5. In Settings, enter the native P2Play daemon address, e.g.
   `http://127.0.0.1:8080`, adjusting the port to the actual deployment, and its
   bearer API token. The default is loopback; in containers loopback means the
   **Silo container**, so use a reachable numeric service address instead.
6. Save, then open Lists or run Search to verify the connection. An empty token
   field keeps the existing token; the explicit checkbox clears it. To change a
   token, enter the replacement. The stored token is never pre-filled or returned.

## Android host and embedded-core boundary

The pinned Silo source includes the Goja plugin manager and its plugin HTTP routes.
The upstream Silo.3 release also publishes Android server binaries (including
arm64 and arm7). This makes an Android Silo host technically plausible, but the
MediaMesh plugin has **not yet been installed and exercised on a real Android
Silo host**; do not mark this deployment as verified until that test is done.

P2Play itself has an Android APK build for `arm64-v8a` and `armeabi-v7a`. The APK
packages the native `p2playd` executable and launches it through an Android
foreground `ServerService` (default HTTP port 8090). This is a standalone server
service, not the future `pPlay_embedded` mode.

The current MediaMesh Silo plugin uses the host's synchronous `ts.http` binding
to call a configured P2Play daemon. It can therefore use an external P2Play
service on the same Android device (for example `http://127.0.0.1:8090`) or a
remote daemon, provided the service is reachable. The plugin runtime does not
expose process creation or a P2Play C API binding: JavaScript cannot start,
stop, or link `p2play_core` by itself. A Silo-managed local core would require a
new host-level launcher/lifecycle bridge and separate packaging, not merely a
plugin change.

Android acceptance remains open:
- install the ZIP through the Android Silo plugin manager and confirm plugin
  routes/menu registration;
- run the P2Play Android foreground service on the same device and test
  authenticated calls from the plugin to its loopback HTTP endpoint;
- verify service stop/restart, permissions, background restrictions and
  persistence on a real device.

Sources: [Silo.3 Android server assets](https://github.com/YouROK/TorrServer/releases/tag/Silo.3),
[pinned Silo plugin runtime](https://github.com/YouROK/TorrServer/tree/3610db846fd2132f06aa626a4cb94f4a0cfa69ae/internal/plugin),
and P2Play's `android/app` / `ci/android-apk.sh`.

## Data flow and permissions

| Plugin function | Native daemon API | Silo permission |
| --- | --- | --- |
| Search, optional tag, bounded limit | `GET /api/catalog/search?q=&tag=&limit=` | rank 10+ |
| Re-read chosen record | `GET /api/catalog/:infohash` | rank 10+ |
| Subscribed lists | `GET /api/lists` | rank 10+ |
| Network list discovery | `GET /api/lists/discover` | rank 10+ |
| Subscribe / request refresh | `POST /api/lists/:pubkey` with `{}` | rank 50+ |
| Change daemon URL/token | Plugin-private BoltDB settings | rank 50+ |
| Add release | Server-side record lookup, then `ts.torrent.add` | rank 10+, own library |

The daemon token defines the plugin's daemon-side access, not the Silo user's
individual daemon identity. All Silo members rank 10+ share that daemon's catalog
view. Subscribe requires the daemon token to have admin rights; Silo additionally
checks the caller's administrator rank. Use a dedicated daemon identity and do
not use a publisher token if those extra privileges are unnecessary.

A result may originate from a catalog card with a `torrs://` link. The plugin does
not hand that unsupported scheme to Silo or trust a browser-supplied magnet. It
re-fetches the selected canonical record by hash, constructs a valid v1 magnet
from its hash/title/trackers, and calls **native** `ts.torrent.add`, explicitly
setting `user_id` from `req.user.id` and `save_to_db: true`. Browser-supplied user
IDs, URLs, titles or torrent options cannot override the native call. This adds
the release to the user's Silo library; playback remains the native Silo UI's
responsibility. There is no browser-to-daemon transfer or automatic external player.

## Security boundaries

- The persisted bearer token stays in Silo's server-side, plugin-prefixed BoltDB
  storage. Browsers can submit a replacement token over HTTPS, but no response,
  HTML, URL, error detail or frontend storage contains the stored token. Daemon
  requests use an Authorization header, never a token query parameter. Protect
  Silo's database, backups and the host OS: plugin storage is **not encrypted**.
- Dynamic pages and API routes authenticate and reject guests/banned users.
  Static JS/CSS contains no configuration or credentials and may be public.
  Missing authentication produces 401; a guest/non-admin gets 403. The upstream
  JS host does not expose whether open mode is enabled: the operator must enforce
  the non-empty owner password prerequisite; the plugin cannot detect it safely.
- All mutations require JSON and a per-user HMAC CSRF header obtained from the
  authenticated same-origin session endpoint. Tokens rotate on plugin reload.
  Cross-site Fetch Metadata is rejected; no CORS permission is granted. This adds
  protection to Silo's original SameSite=Lax cookie without modifying middleware.
  Do not configure a reverse proxy to allow credentialed cross-origin access to
  these endpoints. Like Silo itself, this plugin trusts other installed same-origin
  plugins and the Silo UI; it is not a sandbox against a malicious plugin.
- The daemon URL is an intentional **administrator-only outbound-request
  capability**. Only http(s) origins with canonical dotted numeric IPv4, bracketed
  IPv6, or literal `localhost` are accepted. No DNS names, URL credentials, path,
  query, fragment, alternative numeric IPv4 encodings or header control characters.
  Only the fixed catalog/list endpoints can be requested. Default is loopback;
  administrators may explicitly select other numeric addresses, including remote
  ones. This is not a private-IP denylist and is not safe to delegate to untrusted
  administrators. `localhost` must resolve to loopback on the Silo host.
- **Upstream HTTP limitations:** the shared Go client follows redirects and has no
  JS redirect-control/transport API. Therefore only configure a trusted daemon
  that does not redirect. A JS origin validator cannot enforce a post-redirect
  destination policy; egress firewall rules are needed for hard SSRF confinement.
  Never configure arbitrary third-party URLs. Use HTTPS for remote daemons.
- Native daemon/host exceptions and daemon response bodies are not returned to
  the browser. Errors report only fixed messages and HTTP status codes. Daemon
  responses are projected onto explicit catalog/list fields, not transparently
  proxied. JS uses 4-second outbound contexts (host client also has a 5-second
  ceiling); search is bounded to 100 results (UI requests 50), lists to 1000.
- The host reads HTTP responses fully **before** JS gets `response.body`; the
  plugin's 4 MiB response check is not a transport memory cap. Use a trusted,
  bounded daemon/reverse proxy. The goja VM serializes handlers, so a slow daemon
  can block this plugin until that request's timeout. There is no background poll
  loop, automatic retry storm, or host changes to hide those constraints.
- All catalog and list strings are rendered with DOM `textContent`, never
  `innerHTML`; no inline event handlers/scripts, remote fonts, HTML descriptions,
  external posters or daemon-supplied links are rendered. The page sets restrictive
  CSP, no-store, nosniff and no-referrer headers. External posters are deliberately
  not forwarded to the native library. Allowed http/https/udp trackers are native
  torrent metadata; adding a release intentionally permits normal torrent network
  activity under the Silo engine's own policies.

## Explicit functional limits

- **Remove subscription is unavailable:** native `DELETE /api/lists/:pubkey`
  exists, but the pinned Silo JS `ts.http` exposes only GET/POST. There is no fake
  DELETE override, localhost core API workaround, method-tunnelling query, or Go
  patch. Use the daemon's native authorized UI/API for deletion. The plugin does
  not expose a misleading remove button.
- **Refresh means re-subscribe** through the supported `POST /api/lists/:pubkey`
  path, not an invented `/refresh` endpoint. Installation/download happens
  asynchronously in the native daemon; reload the list view after it completes.
- v2/64-character torrent hashes cannot be transferred through this pinned Silo
  parser; results remain visible with Add disabled. Hybrid catalogs can use v1.
- Discovery, subscription and search need the existing native daemon and its
  network connectivity. This plugin does not implement a mesh engine or verify
  signatures itself; verified catalogs remain the native daemon's responsibility.
- Publishing, filesystem import, media-pack display, subscription deletion,
  selective-shard policy editing, and automatic playback are not implemented in
  this plugin. No token is sent to the browser to make those features appear to
  work around missing host APIs.

## Tests

Run from any directory, with Python 3, git and Go **1.26+** available:

```sh
python3 -I /path/to/P2Play/tools/silo-plugin-test.py
```

To use a clean checkout already at the exact commit (not modified by the tests):

```sh
python3 -I /path/to/P2Play/tools/silo-plugin-test.py \
  --upstream /path/to/TorrServer --go /path/to/go
```

The runner checks the pin, creates an isolated temporary tree, injects
`tools/silo-plugin-test.go` only into that copy, and runs real upstream
`go test ./internal/plugin -run '^TestMediaMesh' -count=1 -v`. It modifies neither
the P2Play core nor the supplied upstream checkout. Use `--race` to enable Go race detection (also requires a C compiler). Go's normal module download
and cache are used. Initial dependency downloads require network access.

Verified with Go **1.26.11** against the above pin: all five tests passed both
with the normal runner and with `--race` (no race reports).

1. Real `NewJSRuntime`/goja, manifest allowlist, real VFS HTML, real Gin handler
   request/response conversion, Silo cookie authentication, rank checks and CSRF.
2. URL/header-injection validation, administrator settings, real BoltDB persistence,
   token retention/clear semantics, no credentials in any test response.
3. Real host `ts.http` against an `httptest` daemon: bearer header, escaped search,
   catalog/detail/list/discovery/subscription, malformed/failed/offline daemon.
4. Native transfer builds a magnet accepted by **actual** upstream
   `torrent.ParseTorrentSpec`, preserves permitted trackers and passes exactly the
   authenticated user's ID and save option; injected browser IDs/URLs are ignored.
5. Actual `Manager.InstallPlugin` ZIP loader and asynchronous plugin startup.

**Test boundary:** the Go host-contract suite still replaces `ts.torrent.add` with a
recording function in one focused transfer test. It verifies the exact native call
without starting a download. The separate `tools/silo-live-test.py` acceptance
test builds this pinned Silo server and runs the installed plugin against real
P2Play daemons, a real Silo torrent engine, a local HTTP tracker and a loopback
libtorrent seeder. It verifies discovery, subscription, catalog search, native
torrent addition to the authenticated user's library and byte-for-byte playback
through Silo's authenticated stream API. It uses no public tracker or public DHT.
The test is run by Linux CI; it does not replace the host-contract/race tests.

The live test verifies the complete server-side integration path:
MediaMesh discovery/subscription/search, plugin-triggered torrent addition, the
native Silo torrent engine's transfer from the local seeder, and byte-for-byte
Range streaming. It does **not** test browser playback, codec support, FFmpeg
transcoding, or discovery over the public Internet. The transfer assertion uses
libtorrent's cumulative payload-upload counter rather than the instantaneous
peer count, because a peer can disconnect normally as soon as the requested
range has been served.
Run it manually from the repository root with
`python3 tools/silo-live-test.py build-linux/src/app/p2playd` (Go 1.26+,
Python `libtorrent`, and network access for the pinned upstream checkout and Go
modules are required). The runner intentionally uses the normal Python site
configuration: CI installs `libtorrent` with `pip3`, and Python's isolated
`-I` mode excludes user-site packages. Browser files also pass `node --check`;
a browser layout/screenshot test has not been run.


The shared page includes `lang/en.json` and `lang/ru.json` as separately served assets. The Silo session endpoint also returns the authenticated user ID so language/theme preferences can be scoped to that user in browser storage.
