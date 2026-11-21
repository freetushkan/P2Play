#include "auth.hpp"

#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>

#include "../../third_party/nlohmann/json.hpp"

namespace p2p {

    using nlohmann::json;

    namespace {
        json readJson(const std::string &path) {
            std::ifstream in(path);
            if (!in) {
                return json();
            }
            std::stringstream ss;
            ss << in.rdbuf();
            return json::parse(ss.str(), nullptr, false);
        }

        // as Silo: "ts_" and 24 letters or digits
        std::string newToken() {
            static const char *chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
            std::random_device rd;
            std::string out = "ts_";
            for (int i = 0; i < 24; i++) {
                out += chars[rd() % 62];
            }
            return out;
        }

        std::string base64Decode(const std::string &in) {
            static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out;
            int val = 0, bits = -8;
            for (unsigned char c : in) {
                size_t pos = chars.find(static_cast<char>(c));
                if (pos == std::string::npos) {
                    break;
                }
                val = (val << 6) + static_cast<int>(pos);
                bits += 6;
                if (bits >= 0) {
                    out += static_cast<char>((val >> bits) & 0xFF);
                    bits -= 8;
                }
            }
            return out;
        }

        bool equalTime(const std::string &a, const std::string &b) {
            if (a.size() != b.size()) {
                return false;
            }
            unsigned char diff = 0;
            for (size_t i = 0; i < a.size(); i++) {
                diff |= static_cast<unsigned char>(a[i] ^ b[i]);
            }
            return diff == 0;
        }

        std::string cookie(const HttpRequest &req, const std::string &name) {
            std::string all = req.header("cookie");
            size_t pos = 0;
            while (pos < all.size()) {
                size_t end = all.find(';', pos);
                std::string item = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                item.erase(0, item.find_first_not_of(' '));
                if (item.compare(0, name.size() + 1, name + "=") == 0) {
                    return item.substr(name.size() + 1);
                }
                if (end == std::string::npos) {
                    break;
                }
                pos = end + 1;
            }
            return "";
        }
    }

    void Auth::load(const std::string &dir) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_path = dir + "/auth.json";
        m_users.clear();
        bool changed = false;
        json j = readJson(m_path);
        if (j.is_object() && j.contains("users") && j["users"].is_array()) {
            for (const auto &u : j["users"]) {
                User user;
                user.name = u.value("name", "");
                user.password = u.value("password", "");
                user.token = u.value("token", "");
                if (!user.name.empty()) {
                    m_users.push_back(user);
                }
            }
        }
#if P2P_COMPAT_MATRIX
        // MatriX's accounts, as it keeps them
        json accs = readJson(dir + "/accs.db");
        if (accs.is_object()) {
            for (auto it = accs.begin(); it != accs.end(); ++it) {
                bool known = false;
                for (const User &u : m_users) {
                    known |= u.name == it.key();
                }
                if (!known && it.value().is_string()) {
                    m_users.push_back({it.key(), it.value().get<std::string>(), ""});
                    changed = true;
                }
            }
        }
#endif
        for (User &u : m_users) {
            if (u.token.empty()) {
                u.token = newToken();
                changed = true;
            }
        }
        if (changed) {
            save();
        }
    }

    void Auth::save() const {
        json users = json::array();
        for (const User &u : m_users) {
            users.push_back({{"name", u.name}, {"password", u.password}, {"token", u.token}});
        }
        std::string tmp = m_path + ".tmp";
        std::string text = json{{"users", users}}.dump(2);
        if (FILE *f = std::fopen(tmp.c_str(), "wb")) {
            bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
            ok = std::fclose(f) == 0 && ok;
            if (ok && std::rename(tmp.c_str(), m_path.c_str()) != 0) {
                std::remove(m_path.c_str());
                std::rename(tmp.c_str(), m_path.c_str());
            }
        }
    }

    bool Auth::required() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_users.empty();
    }

    bool Auth::basicEnabled() const {
#if P2P_COMPAT_MATRIX
        return true;
#else
        return false;
#endif
    }

    std::string Auth::identify(const HttpRequest &req) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string token = req.param("token");
        const std::string header = req.header("authorization");
        if (token.empty() && header.compare(0, 7, "Bearer ") == 0) {
            token = header.substr(7);
        }
        if (token.empty()) {
            token = cookie(req, "p2play_token");
        }
        if (token.empty()) {
            token = cookie(req, "silo_token");
        }
        if (!token.empty()) {
            for (const User &u : m_users) {
                if (equalTime(u.token, token)) {
                    return u.name;
                }
            }
            return "";
        }
#if P2P_COMPAT_MATRIX
        if (header.compare(0, 6, "Basic ") == 0) {
            std::string pair = base64Decode(header.substr(6));
            size_t colon = pair.find(':');
            if (colon != std::string::npos) {
                const std::string name = pair.substr(0, colon), password = pair.substr(colon + 1);
                for (const User &u : m_users) {
                    if (u.name == name && !u.password.empty() && equalTime(u.password, password)) {
                        return u.name;
                    }
                }
            }
        }
#endif
        return "";
    }

    std::string Auth::login(const std::string &name, const std::string &password) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const User &u : m_users) {
            if (u.name == name && !u.password.empty() && equalTime(u.password, password)) {
                return u.token;
            }
        }
        return "";
    }

    std::string Auth::tokenOf(const std::string &name) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const User &u : m_users) {
            if (u.name == name) {
                return u.token;
            }
        }
        return "";
    }

}
