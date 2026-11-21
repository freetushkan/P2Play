# P2Play

All-in-one torrent streamer with a decentralized catalog (MediaMesh), for Linux, Switch, PS4, PS5 and later pPlay.

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
| PS5 | send `p2spike.elf` to elfldr (port 9021) | `/data/p2play` | create `/data/p2play/stop` |

The log goes to `spike.log` in the data directory (PS4: also klog, PS5: klog and a toast with the URL).
Downloads land in `downloads/` there.

## Build

The same SDKs as pPlay, see `.github/workflows/spike.yml`. Boost is header-only:

    ci/boost-headers.sh 1.86.0 /opt/boost-headers
    cmake -S . -B build -DBOOST_INCLUDE_DIR=/opt/boost-headers [-DPLATFORM_SWITCH=ON | -DPLATFORM_PS4=ON]
    cmake --build build --target p2spike_release

PS5: configure with `/opt/ps5-payload-sdk/bin/prospero-cmake ... -DPLATFORM_PS5=ON`.

libtorrent is the upstream tag in `third_party/libtorrent`; `patches/libtorrent/*.patch` are applied
at configure time. Change libtorrent only through those patches.

Local check without DHT (a seeder on 127.0.0.1, byte ranges compared with the source):

    pip install libtorrent
    tools/local-test.py build/src/spike/p2spike

## License

GPL-3.0. libtorrent is BSD-3-Clause, Boost is BSL-1.0.
