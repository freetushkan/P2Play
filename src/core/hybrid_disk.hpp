#pragma once

// The session's storage: the RAM cache for streamed torrents, files on disk for the torrents
// saved under diskPrefix (the MediaMesh list versions: kept and seeded whole).

#include <atomic>
#include <memory>
#include <string>

#include <libtorrent/session_params.hpp>

namespace p2p {

    lt::disk_io_constructor_type hybridDisk(lt::disk_io_constructor_type ram, std::string diskPrefix,
                                          std::shared_ptr<std::atomic<bool>> diskTorrents);

}
