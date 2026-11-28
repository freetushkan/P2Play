# P2Play

All-in-one torrent streamer with a decentralized catalog (MediaMesh), for Linux, Android, Switch, PS4, PS5 and pPlay.

Stage 4 development: the external pPlay client, account management and per-user playback progress
are integrated with the signed MediaMesh catalog, selective replication, media packs, shards and
publisher transport. The native P2Play API is enabled by default. This does not claim completion
of every roadmap condition; see the implementation boundaries below.

## Running

The server answers on port **8660**: `http://<device ip>:8660`. An optional nested URL prefix can be set with `--base-path` (for example `/some_sub_dir/more_path/ololo`); without it, routes are served at the origin root.

| Platform | Run | Data directory | Quit |
| --- | --- | --- | --- |
| Linux | `p2playd [--data DIR] [--port N] [--base-path /prefix/path]`, or the systemd unit `p2playd.service` | `$P2PLAY_DATA`, else `~/.local/share/p2play` | Ctrl+C / SIGTERM |
| Android | `p2play-android.apk`: allow all files access; the server starts with the app (a foreground service). The app talks to its server as the owner (the token of its `auth.json`). A magnet or a `torrs://` link typed, pasted, opened or shared with the app is added and its largest media file opened in a player; "Share torrs://" shares the newest torrent's link; `adb shell am start -d "magnet:?..." com.freetushkan.p2play/.MainActivity` | `/sdcard/.p2play` as pPlay's `.pplay` (without the access: `Android/data/com.freetushkan.p2play/files`) | Stop |
| Switch | `p2play.nro` from hbmenu (application mode, not the album) | `sdmc:/switch/p2play` | `+` |
| PS4 | install the pkg, launch | `/data/p2play` | create `/data/p2play/stop` |
| PS5 | install `P2Play-PS5.pkg` (elfldr / etaHEN running), launch: the package sends `p2play.elf` to elfldr and closes, the payload starts itself again as a big application (pPlay's hbldr: `/data/p2play/bin/p2play.elf`) with the log on the screen, or stays a payload without a screen if that fails; or send the ELF to port 9021 yourself | `/data/p2play` | launch it again (the running server stops; the next launch starts the new one, after an update too), or create `/data/p2play/stop` |

The log: `p2play.log` in the data directory (on the screen on Switch, PS4 and PS5, with the address on top; Android: in the app; PS4/PS5: klog too).
A crash: `crash.log` there (every platform, see Layout); an uncaught exception also writes its reason to
`p2play.log`.

## API

The standalone daemon exposes one native REST API under `/api/v1`, with per-user
Bearer tokens and an HttpOnly same-origin `p2play_token` cookie for the web UI.
The native API and MediaMesh catalog operations share this versioned namespace.

| Route | Purpose |
| --- | --- |
| `POST /api/v1/auth/login`, `GET /api/v1/auth/me`, `POST /api/v1/auth/logout` | Login, session check and logout |
| `GET/POST /api/v1/users`, `PUT/DELETE /api/v1/users/{id}` | Rank-based user management |
| `GET /api/v1/library/torrents`, `GET /api/v1/library/categories` | The authenticated user's library |
| `POST /api/v1/torrents` | Add a magnet, supported link, `torrs://` card or multipart `.torrent` |
| `GET/PUT/DELETE /api/v1/torrents/{hash}` | Read, update or remove a library membership |
| `GET /api/v1/torrents/{hash}/files/{index}/stream` | Authenticated Range-capable media stream |
| `GET /api/v1/torrents/{hash}/cover`, `GET /api/v1/torrents/{hash}/poster` | Server-generated cover and cached user/card poster |
| `GET/POST /api/v1/lists`, `GET /api/v1/catalog/search` | MediaMesh catalog and list operations |
| `GET /api/v1/library/export`, `POST /api/v1/library/import` | Portable `.torrs` library transfer |

File indices in the native API are zero-based. Playback progress and watched state
are stored per user and per torrent file. The Silo MediaMesh plugin is a separate
host integration under `plugins/mediamesh/`; it does not turn the daemon into a
different server API.

The `torrs://` URI uses the TorrsHash-compatible base62/zlib payload containing
the infohash and optional title, poster, category, size and trackers. P2Play parses
the URI when adding a torrent. If it contains a poster URL that passes the server's
outbound-fetch policy, the image is cached locally and returned as a same-origin
poster URL. A failed or unsafe poster fetch does not block the torrent add.

### MediaMesh lists (stage 3)

A list is an ed25519 key; a version is a signed manifest and append-only segments of records,
in `lists/<pubkey>/` of the data directory (the format: `src/core/mesh.hpp`). The lists of a node are
checked (signature, seq, segment hashes, limits), merged and searched.

Over the network: a version is a v2 torrent of the list's directory, kept on disk and seeded (the session's
storage is RAM for streams, files for `lists/`); the pointer to the current version is a BEP 44 mutable item
under the list's key (salt `mediamesh.v1`, value: the version's infohash and the name, seq: the version).
Subscribing by the key: the pointer from the DHT, the version downloaded, checked, seeded. Every node puts
the pointers of its lists again every 30 minutes and asks for newer versions: a list lives on without its
releaser.

The directory swarm: a fixed tiny torrent (one file `mediamesh.v1`, v1, 16 KiB pieces) every node joins and
nobody holds the data of (a seed would meet no other seed). Its peers speak the `mediamesh` BEP 10 extension:
`have`, the lists a node holds and their versions, sent at the handshake and again when they change. A node
learns so the lists of the network and how many nodes hold each (`GET /api/v1/lists/discover`, the most held
first: the rating), and asks the DHT at once for a newer version of a list it holds.

A catalog node (`P2PCatalogOnly` in the settings): the lists, the directory and the search, no streamer (the
torrent and stream routes answer 404): beside Silo, for its plugin, or on a server holding lists round the
clock. `P2PDhtBootstrap` (settings, `host:port,...`): a DHT of one's own (tests, a LAN).

The extension now exchanges signed `get`/`item` pointers, independent of DHT availability;
receivers check the BEP 44 signature, salt, sequence and canonical value before fetching.
[Peer exchange format and bounds](docs/peer-items.md) describes persistence and relay behavior.

[Media packs, shards and snapshots](docs/media-shards.md) documents the signed format and API:
`POST /api/v1/publish/:pk/media/{description,poster}`, `POST .../snapshot`,
`GET /api/v1/catalog/:ih/{description,poster}` and `GET/POST /api/v1/lists/:pk/policy`.
Replication authenticates the manifest first, downloads only admitted record shards, and queues
missing media in a bounded background worker. A pending media response is 503; retry after the
pack is installed. `mirror_media` defaults to false. Partial holders preserve the original torrent
identity across restarts and never use seed mode. Lazy retrieval currently downloads the **whole
containing pack**, not an individual object's byte range. Budgets cover retained payload per list,
not total filesystem usage or temporary working copies. Catalog lists/policies remain node-wide,
not separate subscription replicas for every API user.

[Publisher SOCKS5 and namespace/VPN deployment](docs/publisher-proxy.md) covers startup-only
`PublisherProxy`, UDP ASSOCIATE, credential handling and network boundaries. It applies to the
whole libtorrent session. It is not an anonymity guarantee; use OS egress controls when required.
HTTP(S) torrent URL fetching is disabled in this mode rather than bypassing the proxy.

[The Silo plugin](docs/silo-plugin.md) lives in `plugins/mediamesh/`, pinned and tested against
upstream Silo commit `3610db846fd2132f06aa626a4cb94f4a0cfa69ae`. Its search/list UI keeps the daemon
token server-side and adds selected v1 torrents to the authenticated Silo user's library. The
pinned Silo host lacks DELETE HTTP and v2 torrent parsing: subscription removal stays in the
daemon API and v2-only results cannot be added. No upstream Silo patch is required.
Native Linux tests cover storage with ASan/UBSan, raw peer messages, real selective transfers,
restarts and a local SOCKS5 fixture; platform SDK/runtime validation remains outstanding.

| Route | Description |
| --- | --- |
| `GET /api/v1/catalog/search?q=&tag=&limit=&lists=` | every word in the title or tags (any case, Latin and Cyrillic, ё = е); optional comma-separated list public keys restrict the results to those catalogs; each result includes its `torrs://` link and source lists |
| `GET /api/v1/catalog/:infohash` | a record and its lists |
| `GET /api/v1/lists`, `DELETE /api/v1/lists/:pubkey` | the lists of this node (`net`: seeded, being fetched); a received one dropped |
| `GET /api/v1/lists/discover` | the lists the directory swarm told of: `holders`, `seq`, `subscribed` |
| `POST /api/v1/lists/:pubkey` | subscribe by the key (`{"peers": ["ip:port"]}`: optional hints): 202 |
| `POST /api/v1/lists/import` | `{"dir"}`: a version directory taken (admins) |
| `POST /api/v1/publish` | `{"name","description"}`: a new list, its key in `keys/` (admins) |
| `POST /api/v1/publish/:pubkey/records`, `.../release`, `GET .../status` | NDJSON records (`ih`, `title`, `size`, `files`, `date`, `tags`, `trackers`, `poster`; `{"del": ih}`), the next version |

### Users and authentication

All accounts live in `auth.json` in the data directory and use the same
`/api/v1` token model. `POST /api/v1/auth/login` verifies the username/password
and returns the existing API token; the browser receives it in an HttpOnly
`p2play_token` cookie. Tokens are long-lived API credentials, not short login
sessions. A user may regenerate their token, and an administrator may regenerate
tokens of lower-rank accounts.

The owner (`owner`, rank 100) is created on first start. Until it has a password,
the first login as `owner` with a password sets it. Other users are managed via
`/api/v1/users`: admins rank 50–99, users 10–49 and guests 1–9. Each user has a
separate library and playback progress; a torrent with no remaining library
membership is forgotten after it closes.

P2Play discovery uses the public `GET /api/v1/system/ping` and
`GET /api/v1/system/version` endpoints. The daemon does not expose a parallel
unversioned API at the site root.

The library memberships are stored in `torrents.json`, accounts in `auth.json`, resume data and uploaded `.torrent` files under `torrents/`, playback progress in `viewed.json`, and settings in `settings.json`. Torrents are opened when used and closed after
`TorrentDisconnectTimeout` without a reader; those not in the library are then forgotten.

Storage: RAM by default: pieces live in memory (`CacheSize`, 128 MB). Only the pieces
around each reader (`ReaderReadAHead` % of the cache, at most half of it) and both ends of the files being
played are downloaded; past the cache the oldest pieces are dropped, those of the readers' windows last, and
libtorrent downloads them again when wanted (libtorrent patch 0006). `UseDisk`: pieces on disk
(`TorrentsSavePath`, else `downloads/`), applied at the next start.

## Build

The same SDKs as pPlay, see `.github/workflows/build.yml`. Boost is header-only:

    ci/boost-headers.sh 1.86.0 /opt/boost-headers
    cmake -S . -B build -DBOOST_INCLUDE_DIR=/opt/boost-headers [-DPLATFORM_SWITCH=ON | -DPLATFORM_PS4=ON | -DPLATFORM_ANDROID=ON -DANDROID_ABI=armeabi-v7a]
    cmake --build build --target p2play_release

The optional HTTP base path is also available to embedded hosts through `p2play_config.http_prefix`. The bundled libtorrent build forces `i2p=OFF`; P2Play does not use I2P as a runtime transport or privacy feature.

Android: `ci/android-apk.sh <boost headers>` builds p2playd with the NDK (`ANDROID_NDK_HOME`, Android 6+) for
arm64-v8a and armeabi-v7a and packages it into one APK (`android/`) as `lib/<abi>/libp2playd.so`. Android extracts it to the native library directory and runs it as a foreground service.
The CI signs with the `ANDROID_KEYSTORE_*` secrets (the same as pPlay) when they are set, else with a new debug key.

PS5: configure with `/opt/ps5-payload-sdk/bin/prospero-cmake ... -DPLATFORM_PS5=ON`. The package is a separate
project built with the PS4 toolchain: `src/app/ps5/launcher` (`-DP2PLAY_ELF=<the PS5 build>/p2play.elf`).

Layout:

- `src/core`: the core, a module (a static library, C API in `p2play.h`). It has no platform code: the host
  gives it the data directory and a log callback, asks `p2play_active_streams()` to keep the device awake,
  and installs its own crash handlers. Inside pPlay these are pPlay's (its Platform, CrashLog, log, data
  directories); the core's HTTP port can be any free one there (`http_port = -1`).
- `src/app`: the standalone host, pPlay's platform harness for a headless application: crash handlers of every
  platform (`crashlog.h`), the platform name and behaviour (`platform.cpp`: Switch exit request and background
  service, PS4 video playback ticks while streaming and exit through the system, PS5 notifications), the log,
  the data directories. p2playd on Linux and Android, the homebrew on Switch, PS4 and PS5.
- `android/`: the APK around p2playd.
- `tools/`: `ui-test.py` (native API and web UI), `auth-test.py` (tokens and users), `mesh-test.py` / `mesh-meta-test.py` / `meshnet-test.py` (MediaMesh and multi-node tests), `silo-plugin-test.py` / `silo-live-test.py` (pinned Silo host integration), and `make-icon.py` (platform icons).

Debug symbols as pPlay: everything is built with `-g`, the release binaries are stripped, the full ones are
kept in `build/symbols` (CI: `P2Play-Symbols-<platform>`). A crash: `crash.log` in the data directory, then
`llvm-addr2line -f -C -e <symbols> <offset>`.

PS4 (and the PS5 package) link with `cmake/ps4-link.x`: create-fself as pinned by pacbrew loads the data
segment only up to the end of `.data`, so `.data.*` sections must be merged into it. `ci/ps4-check-sections.sh`
fails the build when such sections are left.

libtorrent is the upstream tag in `third_party/libtorrent`; `patches/libtorrent/*.patch` are applied
at configure time. Change libtorrent only through those patches: configure moves a tree it patched
itself (or a clean one) to the current series, and stops on changes made by hand.

Boost headers get `patches/boost/*.patch` from `ci/boost-headers.sh`: on Switch and PS4 asio wakes its
reactor through a loopback socket pair (libnx has no pipe/socketpair). What the console SDK headers
miss is in `compat/` (forced include `compat/p2p_platform.h`, `compat/switch/sys/*.h`).

| Patch | Why |
| --- | --- |
| libtorrent 0001 | PS5: `arc4random_buf`, no `getrandom` |
| libtorrent 0002 | Switch: platform branch (no mmap, fallocate, rlimit; `randomGet`) |
| libtorrent 0003 | PS4: platform branch (no sysctl, mmap, rlimit; `/dev/urandom`) |
| libtorrent 0004 | Switch, PS4: no interface/route enumeration, listen on 0.0.0.0 as on Android 7+ |
| libtorrent 0005 | `try_signal` (SIGBUS around mmap) left out where there is no mmap |
| libtorrent 0006 | `torrent_lost_piece()`: a RAM storage drops a piece, the torrent downloads it again |
| boost 0001 | asio: socket-pair interrupter and no signal blocker on request |

Local native API/web UI regression check (temporary daemon and isolated data directory):

    pip install libtorrent
    python3 -I tools/ui-test.py build/src/app/p2playd

## License

GPL-3.0. libtorrent is BSD-3-Clause, Boost is BSL-1.0.
