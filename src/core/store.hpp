#pragma once

// What outlives a run, in JSON files of the data directory: the torrent library
// (torrents.json, metadata in torrents/<hash>.torrent), the viewed files with their
// position (viewed.json), the settings (settings.json). Written whole, through a
// temporary file. SQLite comes with the catalog (stage 3): its full text index.

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "../../third_party/nlohmann/json.hpp"

namespace p2p {

    struct FileInfo {
        int id = 0;          // from 1, as TorrServer
        std::string path;    // inside the torrent, '/' separated, the torrent's name first
        int64_t length = 0;
    };

    struct TorrentRecord {
        std::string hash;    // hex, lower case
        std::string link;    // the magnet to start it again without the metadata file
        std::string title;
        std::string poster;
        std::string category;
        std::string data;    // free text of the client (TorrServer: JSON with the files)
        std::string name;    // the torrent's own name
        int64_t timestamp = 0;
        int64_t size = 0;
        std::vector<FileInfo> files;
    };

    struct ViewedRecord {
        std::string hash;
        int fileIndex = 0;
        double timecode = 0;  // seconds; 0: viewed, no position
    };

    class Store {
    public:
        explicit Store(std::string dir);

        void load();

        const std::string &dir() const { return m_dir; }

        std::string metadataPath(const std::string &hash) const;

        std::vector<TorrentRecord> torrents() const;

        bool torrent(const std::string &hash, TorrentRecord &out) const;

        void putTorrent(const TorrentRecord &record);

        void removeTorrent(const std::string &hash);

        // hash empty: all
        std::vector<ViewedRecord> viewed(const std::string &hash) const;

        void setViewed(const ViewedRecord &record);

        // fileIndex < 1: every file of the torrent
        void removeViewed(const std::string &hash, int fileIndex);

        nlohmann::json settings() const;

        void setSettings(const nlohmann::json &settings);

    private:
        void saveTorrents() const;

        void saveViewed() const;

        bool writeFile(const std::string &name, const std::string &text) const;

        std::string m_dir;
        mutable std::mutex m_mutex;
        std::vector<TorrentRecord> m_torrents;
        std::vector<ViewedRecord> m_viewed;
        nlohmann::json m_settings = nlohmann::json::object();
    };

}
