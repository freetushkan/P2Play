#pragma once

// What outlives a run, in JSON files of the data directory: the torrent library
// (torrents.json, with the users whose libraries hold each; metadata in torrents/), the
// files played with their position and viewed mark, a user (viewed.json), the settings (settings.json). Written whole, through a
// temporary file. SQLite comes with the catalog (stage 3): its full text index.

#include <cstdint>
#include <map>
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
        std::map<std::string, int64_t> users;   // whose library holds it: user id -> when added
    };

    // a file a user played: the position (MatriX's timecode, Silo's progress) and the viewed
    // mark (Silo's viewed_files; set by hand, or once the position passes the threshold)
    struct ViewedRecord {
        std::string user;     // the user id (auth.hpp), "owner" without users
        std::string hash;
        int fileIndex = 0;    // from 1 (MatriX's); Silo's index is fileIndex - 1
        double timecode = 0;  // the position, seconds
        double duration = 0;  // seconds, 0: unknown
        bool viewed = false;
        int64_t updated = 0;  // unix time
    };

    class Store {
    public:
        explicit Store(std::string dir);

        void load();

        const std::string &dir() const { return m_dir; }

        // a .torrent (uploaded, fetched) and libtorrent's resume data (metadata with the v2
        // piece layers, peers, trackers)
        std::string metadataPath(const std::string &hash) const;

        std::string resumePath(const std::string &hash) const;

        std::vector<TorrentRecord> torrents() const;

        bool torrent(const std::string &hash, TorrentRecord &out) const;

        void putTorrent(const TorrentRecord &record);

        void removeTorrent(const std::string &hash);

        // a user's viewed files (hash empty: all)
        std::vector<ViewedRecord> viewed(const std::string &user, const std::string &hash) const;

        // the record of a file, created if missing (fileIndex >= 1)
        ViewedRecord viewedFile(const std::string &user, const std::string &hash, int fileIndex) const;

        void setViewed(const ViewedRecord &record);

        // fileIndex < 1: every file of the torrent
        void removeViewed(const std::string &user, const std::string &hash, int fileIndex);

        // a user removed: his viewed files and his place in the libraries; the hashes no
        // library holds any more
        std::vector<std::string> removeUser(const std::string &user);

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
