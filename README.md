# P2Play

All-in-one torrent streamer with a decentralized catalog (MediaMesh), for Linux, Android, Switch, PS4, PS5 and later pPlay.

Stage 0 of the roadmap: `p2spike`, a proof that libtorrent runs on every target.

## p2spike

A magnet (or an infohash, or a .torrent path) in, the largest file of the torrent out over HTTP with Range:

    http://<device ip>:8095/stream    the file, for mpv / VLC / pPlay
    http://<device ip>:8095/status    JSON: state, peers, DHT nodes, speed

Linux:

    p2spike "magnet:?xt=urn:btih:..." [--peer ip:port] [--port 6881] [--http 8095] [--readahead 16]

Consoles read the link from `magnet.txt` in the data directory: the link on the first line,
then optional `peer ip:port`, `port n`, `http n`, `readahead mb` lines.

| Platform | Run | Data directory | Quit |
| --- | --- | --- | --- |
| Linux | `p2spike <link>` | `./p2play-data` (`P2SPIKE_DATA`) | Ctrl+C |
| Switch | `p2spike.nro` from hbmenu (application mode, not the album) | `sdmc:/switch/p2play` | `+` |
| PS4 | install the pkg, launch | `/data/p2play` | create `/data/p2play/stop` |
| Android | install `p2spike-android.apk`, paste a magnet, Start; or open a magnet link with it, or `adb shell am start -d "magnet:?..." com.freetushkan.p2play/.MainActivity`. Play opens the stream in a player | app files: `p2play` (log on the screen) | Stop |
| PS5 | install `p2spike-PS5.pkg` (elfldr / etaHEN running), launch: the package sends `p2spike.elf` to elfldr and closes; or send the ELF to port 9021 yourself | `/data/p2play` | create `/data/p2play/stop` |

The log goes to `spike.log` in the data directory (PS4: also klog, PS5: klog and a toast with the URL).
Downloads land in `downloads/` there.

## Build

The same SDKs as pPlay, see `.github/workflows/spike.yml`. Boost is header-only:

    ci/boost-headers.sh 1.86.0 /opt/boost-headers
    cmake -S . -B build -DBOOST_INCLUDE_DIR=/opt/boost-headers [-DPLATFORM_SWITCH=ON | -DPLATFORM_PS4=ON | -DPLATFORM_ANDROID=ON -DANDROID_ABI=armeabi-v7a]
    cmake --build build --target p2spike_release

Android: `ci/android-apk.sh <boost headers>` builds p2spike with the NDK (`ANDROID_NDK_HOME`, Android 6+) for
arm64-v8a and armeabi-v7a and packages it into one APK (`android/`) as `lib/<abi>/libp2spike.so`: the system
extracts it to the native library directory, a foreground service runs it, as TorrServe does with its server.
The CI signs with the `ANDROID_KEYSTORE_*` secrets (the same as pPlay) when they are set, else with a new debug key.

PS5: configure with `/opt/ps5-payload-sdk/bin/prospero-cmake ... -DPLATFORM_PS5=ON`. The package is a separate
project built with the PS4 toolchain: `src/spike/ps5/launcher` (`-DP2SPIKE_ELF=<the PS5 build>/p2spike.elf`).

PS4 (and the PS5 package) link with `cmake/ps4-link.x`: create-fself as pinned by pacbrew loads the data
segment only up to the end of `.data`, so `.data.*` sections must be merged into it. `ci/ps4-check-sections.sh`
fails the build when such sections are left.

libtorrent is the upstream tag in `third_party/libtorrent`; `patches/libtorrent/*.patch` are applied
at configure time. Change libtorrent only through those patches.

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
| boost 0001 | asio: socket-pair interrupter and no signal blocker on request |

Local check without DHT (a seeder on 127.0.0.1, byte ranges compared with the source):

    pip install libtorrent
    tools/local-test.py build/src/spike/p2spike

## License

GPL-3.0. libtorrent is BSD-3-Clause, Boost is BSL-1.0.
