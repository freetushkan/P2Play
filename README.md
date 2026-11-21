# P2Play

All-in-one torrent streamer with a decentralized catalog (MediaMesh), for Linux, Android, Switch, PS4, PS5 and later pPlay.

Stage 1 of the roadmap: the core-streamer. A TorrServer MatriX compatible server: pPlay and Lampa use it
instead of TorrServer, on the same device or over the network.

## Running

The server answers on port **8090** (TorrServer's): `http://<device ip>:8090`.

| Platform | Run | Data directory | Quit |
| --- | --- | --- | --- |
| Linux | `p2playd [--data DIR] [--port N]`, or the systemd unit `p2playd.service` | `$P2PLAY_DATA`, else `~/.local/share/p2play` | Ctrl+C / SIGTERM |
| Android | `p2play-android.apk`: allow all files access; the server starts with the app (a foreground service). A magnet typed, pasted, opened or shared with the app is added and its largest media file opened in a player; `adb shell am start -d "magnet:?..." com.freetushkan.p2play/.MainActivity` | `/sdcard/.p2play` as pPlay's `.pplay` (without the access: `Android/data/com.freetushkan.p2play/files`) | Stop |
| Switch | `p2play.nro` from hbmenu (application mode, not the album) | `sdmc:/switch/p2play` | `+` |
| PS4 | install the pkg, launch | `/data/p2play` | create `/data/p2play/stop` |
| PS5 | install `P2Play-PS5.pkg` (elfldr / etaHEN running), launch: the package sends `p2play.elf` to elfldr and closes; or send the ELF to port 9021 yourself | `/data/p2play` | create `/data/p2play/stop` |

The log: `p2play.log` in the data directory (Switch: also on the screen, PS4/PS5: klog, PS5: a toast with the address).
A crash: `crash.log` there (every platform, see Layout); an uncaught exception also writes its reason to
`p2play.log`.

## API

The TorrServer MatriX API in the part pPlay and Lampa use; file ids start from 1:

| Route | |
| --- | --- |
| `GET /echo` | the version and the platform (`MatriX.141 P2Play/0.1 ps5`) |
| `POST /torrents` | `add` (`link`: magnet, infohash, `http://` .torrent link; `title`, `poster`, `category`, `data`, `save_to_db`), `get`, `set`, `rem`, `drop`, `list`, `wipe` |
| `POST /torrent/upload` | .torrent files (multipart: `file`, `title`, `poster`, `data`, `save`) |
| `GET /stream[/name]` | `?link=&index=` with `play` (the file, Range), `stat`, `preload`, `save`, `m3u`, `fromlast`, `title`, `poster` |
| `GET /play/<hash>/<id>` | the file |
| `POST /viewed` | `set` / `rem` / `list` with `hash`, `file_index`, `timecode` (the position, s) |
| `POST /settings` | `get` / `set` (`sets`) / `def`: TorrServer's BTSets; used: `CacheSize`, `ReaderReadAHead`, `PreloadCache`, `UseDisk`, `TorrentsSavePath`, `TorrentDisconnectTimeout`, `ConnectionsLimit`, rate limits, `DisableUpload`, `DisableDHT`, `DisableUPNP`, `PeersListenPort` |
| `POST /cache` | `get`: the cache of a torrent |
| `GET /playlist?hash=`, `GET /playlistall/all.m3u` | M3U of the media files |

Not yet: `torrs://` links and `https://` .torrent links (stage 3), users (stage 2), DLNA, search.

The library (`torrents.json`; in `torrents/` libtorrent resume data with the metadata, v2 piece layers and peers, and uploaded .torrent files), the viewed files (`viewed.json`) and the settings
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
