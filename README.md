# P2Play

All-in-one torrent streamer with a decentralized catalog (MediaMesh), for Linux, Android, Switch, PS4, PS5 and later pPlay.

Stage 4 development: the external pPlay client, account management and per-user playback progress
are integrated with the signed MediaMesh catalog, selective replication, media packs, shards and
publisher transport. The server remains compatible with TorrServer's MatriX and Silo APIs, while
exposing a separate native P2Play identity endpoint. This does not claim completion of every
roadmap condition; see the implementation boundaries below.

## Running

The server answers on port **8090** (TorrServer's): `http://<device ip>:8090`.

| Platform | Run | Data directory | Quit |
| --- | --- | --- | --- |
| Linux | `p2playd [--data DIR] [--port N]`, or the systemd unit `p2playd.service` | `$P2PLAY_DATA`, else `~/.local/share/p2play` | Ctrl+C / SIGTERM |
| Android | `p2play-android.apk`: allow all files access; the server starts with the app (a foreground service). The app talks to its server as the owner (the token of its `auth.json`). A magnet or a `torrs://` link typed, pasted, opened or shared with the app is added and its largest media file opened in a player; "Share torrs://" shares the newest torrent's link; `adb shell am start -d "magnet:?..." com.freetushkan.p2play/.MainActivity` | `/sdcard/.p2play` as pPlay's `.pplay` (without the access: `Android/data/com.freetushkan.p2play/files`) | Stop |
| Switch | `p2play.nro` from hbmenu (application mode, not the album) | `sdmc:/switch/p2play` | `+` |
| PS4 | install the pkg, launch | `/data/p2play` | create `/data/p2play/stop` |
| PS5 | install `P2Play-PS5.pkg` (elfldr / etaHEN running), launch: the package sends `p2play.elf` to elfldr and closes, the payload starts itself again as a big application (pPlay's hbldr: `/data/p2play/bin/p2play.elf`) with the log on the screen, or stays a payload without a screen if that fails; or send the ELF to port 9021 yourself | `/data/p2play` | launch it again (the running server stops; the next launch starts the new one, after an update too), or create `/data/p2play/stop` |

The log: `p2play.log` in the data directory (on the screen on Switch, PS4 and PS5, with the address on top; Android: in the app; PS4/PS5: klog too).
A crash: `crash.log` there (every platform, see Layout); an uncaught exception also writes its reason to
`p2play.log`.

## API

Two APIs over the same core, the same users and the same progress:

- the native one, under `/api`: the REST scheme of TorrServer's silo branch (Silo), with the viewing
  progress added; a token;
- TorrServer MatriX, as pPlay and Lampa use it; Basic. A compatibility layer
  (`P2P_COMPAT_MATRIX`, CMake, ON): the core and `/api` build without it.

Files are numbered as the originals number them, so ids kept by a client (marks, progress) stay right when it
moves between TorrServer and P2Play: MatriX ids from 1 over every file of the torrent sorted by path
(TorrServer's CompareStrings: `e2` before `e10`), BEP 47 pad files counted; Silo's index is the file's index in
the torrent. Both list the files in TorrServer's order and hide pad files. A record of an earlier P2Play is
numbered again at the start from the metadata kept, its progress moved along by path.

### /api (Silo)

| Route | |
| --- | --- |
| `GET /api/system/ping` | public TorrServer/Silo compatibility probe |
| `GET /api/p2play/version` | public native P2Play identity/version (`name: P2Play`, `version: 0.1`); no compatibility prefix |
| `POST /api/auth/login` | `{"username","password"}` → `{"user","token"}` and the `silo_token` cookie; `POST /api/auth/logout`, `GET /api/auth/me` |
| `GET /api/system/version`, `GET /api/system/stats`, `POST /api/system/shutdown` | shutdown: the owner |
| `GET /api/torrents` | `{"torrents": [status]}`: the user's library |
| `POST /api/torrents` | JSON `{link, title, poster, category, save_to_db}` or multipart with a .torrent `file`; saved by default; 201 |
| `GET` / `DELETE /api/torrents/:hash` | the status (`file_stats` with `id` from 0, `name`, `file_hash`; `viewed_files`; `file_progress`); out of the user's library |
| `PUT /api/torrents/:hash/meta` | `{title, poster, category}` |
| `POST /api/torrents/:hash/wake`, `POST .../files/:idx/preload` | open; preload a file |
| `POST /api/torrents/:hash/files/:idx/viewed`, `POST .../viewed` | `{"viewed": bool}`, a file or all of them |
| `PUT /api/torrents/:hash/files/:idx/progress` | `{"position", "duration"}` (s); viewed past 90 % (new, not in Silo) |
| `GET /api/progress?since=<unix time>` | "continue watching": the user's files with a position, the latest first (new) |
| `GET /api/torrents/:hash/cache`, `.../peers` | 204 when closed |
| `GET /api/stream/:hash/:idx` | the file, Range; `?token=` for players |
| `GET /api/library/export`, `POST /api/library/import` | the library as `.torrs` (a `torrs://` link a line); imported cards are not opened |
| `GET/POST /api/users`, `PUT/DELETE /api/users/:id`, `POST .../regenerate-token`, `POST .../rank`, `GET .../torrents` | Silo's user management by rank |

`torrs://` links (TorrServer's TorrsHash: base62 of zlib(the infohash, title, poster, category, size,
trackers)) are added on both APIs as any other link; the status of `/api` gives each torrent's (`torrs`).
`/api/events` (SSE: `sync`, `card`, `remove`), `/api/ui/menu` (empty: no plugins) and `/api/system/torrent-config`
as Silo, for Silo's clients (TorrServe Silo for Android). Not here: Silo's transcoding and plugins.
Unlike Silo, a stream does not mark a file viewed (players read the end of a file first): the progress does.

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
learns so the lists of the network and how many nodes hold each (`GET /api/lists/discover`, the most held
first: the rating), and asks the DHT at once for a newer version of a list it holds.

A catalog node (`P2PCatalogOnly` in the settings): the lists, the directory and the search, no streamer (the
torrent and stream routes answer 404): beside Silo, for its plugin, or on a server holding lists round the
clock. `P2PDhtBootstrap` (settings, `host:port,...`): a DHT of one's own (tests, a LAN).

The extension now exchanges signed `get`/`item` pointers, independent of DHT availability;
receivers check the BEP 44 signature, salt, sequence and canonical value before fetching.
[Peer exchange format and bounds](docs/peer-items.md) describes persistence and relay behavior.

[Media packs, shards and snapshots](docs/media-shards.md) documents the signed format and API:
`POST /api/publish/:pk/media/{description,poster}`, `POST .../snapshot`,
`GET /api/catalog/:ih/{description,poster}` and `GET/POST /api/lists/:pk/policy`.
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

| Route | |
| --- | --- |
| `GET /api/catalog/search?q=&tag=&limit=&lists=` | every word in the title or tags (any case, Latin and Cyrillic, ё = е); optional comma-separated list public keys restrict the results to those catalogs; each result includes its `torrs://` link and source lists |
| `GET /api/catalog/:infohash` | a record and its lists |
| `GET /api/lists`, `DELETE /api/lists/:pubkey` | the lists of this node (`net`: seeded, being fetched); a received one dropped |
| `GET /api/lists/discover` | the lists the directory swarm told of: `holders`, `seq`, `subscribed` |
| `POST /api/lists/:pubkey` | subscribe by the key (`{"peers": ["ip:port"]}`: optional hints): 202 |
| `POST /api/lists/import` | `{"dir"}`: a version directory taken (admins) |
| `POST /api/publish` | `{"name","description"}`: a new list, its key in `keys/` (admins) |
| `POST /api/publish/:pubkey/records`, `.../release`, `GET .../status` | NDJSON records (`ih`, `title`, `size`, `files`, `date`, `tags`, `trackers`, `poster`; `{"del": ih}`), the next version |

### MatriX

| Route | |
| --- | --- |
| `GET /echo` | public: the version and the platform (`MatriX.141 P2Play/0.1 ps5`) |
| `POST /torrents` | `add` (`link`: magnet, infohash, `http://` .torrent link; `title`, `poster`, `category`, `data`, `save_to_db`), `get`, `set`, `rem`, `drop`, `list`, `wipe` |
| `POST /torrent/upload` | .torrent files (multipart: `file`, `title`, `poster`, `data`, `save`) |
| `GET /stream[/name]` | `?link=&index=` with `play` (the file, Range), `stat`, `preload`, `save`, `m3u`, `fromlast`, `title`, `poster` |
| `GET /play/<hash>/<id>` | the file |
| `POST /viewed` | `set` / `rem` / `list` with `hash`, `file_index`, `timecode`: the position of `/api`'s progress |
| `POST /settings` | `get` / `set` (`sets`) / `def`: TorrServer's BTSets; used: `CacheSize`, `ReaderReadAHead`, `PreloadCache`, `UseDisk`, `TorrentsSavePath`, `TorrentDisconnectTimeout`, `ConnectionsLimit`, rate limits, `DisableUpload`, `DisableDHT`, `DisableUPNP`, `PeersListenPort` |
| `POST /cache` | `get`: the cache of a torrent |
| `GET /playlist?hash=`, `GET /playlistall/all.m3u` | M3U of the media files |
| `GET /download/<MB>` | the speed test: zeros, Range |
| `GET/POST /storage/settings` | the storage (`ram` / `disk`) |
| `GET /shutdown` | stops the server |

`add` and `/stream` fill a title, a poster or a category the torrent has no value for, as MatriX.

### Users

One set of users, `auth.json` in the data directory, required on both APIs; each asks as its original does:
`/api` (Silo) a token (`?token=`, `Authorization: Bearer`, the `silo_token` cookie), the MatriX routes Basic
(`WWW-Authenticate`). The MatriX routes TorrServer leaves open (`/stream` with `play` or `m3u`, `/play`,
`/playlist`) play a torrent already known without a login, as there: players open those links without
credentials. The library, the progress and everything else are one store; the file numbers of each API are
kept apart (MatriX's id, Silo's index).

The owner (`owner`, rank 100) is always there. Until it has a password, the first login as `owner` with a
password sets it, on either API (Silo's client: log in as `owner`; pPlay or Lampa: `owner` and the password in
the network's credentials): a console has no other place to set it. Or by hand, before the first start:
`{"users": [{"username": "owner", "password": "secret"}]}` in `auth.json` (hashed at the start).
`GET /api/system/auth`: whether the password is set.

The other users are made through `/api/users`: a user manages those of a lower rank (admins 50-99, users 10-49,
guests 1-9). Each user has their own library and progress; a torrent no library holds is forgotten once closed.

A client works with a server through one of the two APIs: their file numbers differ (MatriX's id, Silo's index).

Discovery: pPlay and TorrServe find a TorrServer on port 8090 by `/echo` (public, `MatriX.141 P2Play/...`:
MatriX's version prefix); Silo's clients by the public `/api/system/ping`.

Not yet: `https://` .torrent links, DLNA, `/ffp` and the remaining roadmap features
(including Continue Watching UI and the future embedded pPlay mode).

The library (`torrents.json`, with whose libraries hold each torrent; the users in `auth.json`; in `torrents/` libtorrent resume data with the metadata, v2 piece layers and peers, and uploaded .torrent files), the progress (`viewed.json`) and the settings
(`settings.json`) are JSON files in the data directory. Torrents are opened when used and closed after
`TorrentDisconnectTimeout` without a reader; those not in the library are then forgotten.

Storage: RAM by default, as TorrServer: pieces live in memory (`CacheSize`, 128 MB). Only the pieces
around each reader (`ReaderReadAHead` % of the cache, at most half of it) and both ends of the files being
played are downloaded; past the cache the oldest pieces are dropped, those of the readers' windows last, and
libtorrent downloads them again when wanted (libtorrent patch 0006). `UseDisk`: pieces on disk
(`TorrentsSavePath`, else `downloads/`), applied at the next start.

## Build

The same SDKs as pPlay, see `.github/workflows/build.yml`. Boost is header-only:

    ci/boost-headers.sh 1.86.0 /opt/boost-headers
    cmake -S . -B build -DBOOST_INCLUDE_DIR=/opt/boost-headers [-DPLATFORM_SWITCH=ON | -DPLATFORM_PS4=ON | -DPLATFORM_ANDROID=ON -DANDROID_ABI=armeabi-v7a]
    cmake --build build --target p2play_release

Android: `ci/android-apk.sh <boost headers>` builds p2playd with the NDK (`ANDROID_NDK_HOME`, Android 6+) for
arm64-v8a and armeabi-v7a and packages it into one APK (`android/`) as `lib/<abi>/libp2playd.so`: the system
extracts it to the native library directory, a foreground service runs it, as TorrServe does with its server.
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
- `tools/`: the tests of the core, the users, `/api` and the lists (`meshnet-test.py`: three nodes with a DHT of their own) (a local seeder, the CI runs them), `make-icon.py` (the icons of every platform).

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

Local check without DHT (a seeder on 127.0.0.1; the API as pPlay and Lampa use it, byte ranges compared
with the source, a restart with the library kept):

    pip install libtorrent
    tools/core-test.py build/src/app/p2playd [--cache16 | --disk]

## License

GPL-3.0. libtorrent is BSD-3-Clause, Boost is BSL-1.0.
