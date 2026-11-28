#include "auth.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>

#include <libtorrent/hasher.hpp>

namespace p2p {

    using nlohmann::json;

    namespace {
        constexpr int kRounds = 20000;

        json readJson(const std::string &path) {
            std::ifstream in(path);
            if (!in) {
                return json();
            }
            std::stringstream ss;
            ss << in.rdbuf();
            return json::parse(ss.str(), nullptr, false);
        }

        std::string randomString(size_t n) {
            static const char *chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
            std::random_device rd;
            std::string out;
            for (size_t i = 0; i < n; i++) {
                out += chars[rd() % 62];
            }
            return out;
        }

        // 24 random alphanumeric characters, namespaced to identify a P2Play API key
        std::string newToken() {
            return "p2p-" + randomString(24);
        }

        std::string hex(const char *data, size_t size) {
            static const char *digits = "0123456789abcdef";
            std::string out;
            for (size_t i = 0; i < size; i++) {
                out += digits[(static_cast<unsigned char>(data[i]) >> 4) & 15];
                out += digits[static_cast<unsigned char>(data[i]) & 15];
            }
            return out;
        }

        std::string digest(const std::string &salt, const std::string &password, int rounds) {
            lt::sha256_hash h = lt::hasher256(salt + ":" + password).final();
            for (int i = 1; i < rounds; i++) {
                lt::hasher256 next(h.data(), static_cast<int>(h.size()));
                next.update(password);
                h = next.final();
            }
            return hex(h.data(), h.size());
        }

        std::string hashPassword(const std::string &password) {
            if (password.empty()) {
                return "";
            }
            const std::string salt = randomString(16);
            return "sha256$" + std::to_string(kRounds) + "$" + salt + "$" + digest(salt, password, kRounds);
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

        bool checkPassword(const std::string &stored, const std::string &password) {
            // sha256$<rounds>$<salt>$<hash>
            size_t a = stored.find('$'), b = stored.find('$', a + 1), c = stored.find('$', b + 1);
            if (stored.compare(0, a, "sha256") != 0 || c == std::string::npos || password.empty()) {
                return false;
            }
            int rounds = std::atoi(stored.substr(a + 1, b - a - 1).c_str());
            if (rounds < 1 || rounds > 1000000) {
                return false;
            }
            return equalTime(digest(stored.substr(b + 1, c - b - 1), password, rounds), stored.substr(c + 1));
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

        std::string isoTime(int64_t t) {
            char buf[32];
            time_t tt = static_cast<time_t>(t);
            struct tm tm{};
            gmtime_r(&tt, &tm);
            std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
            return buf;
        }
    }

    json User::json(bool withToken) const {
        nlohmann::json j = {{"id", id},
                            {"username", name},
                            {"rank", rank},
                            {"is_banned", banned},
                            {"limits", {{"max_torrents", 0}, {"download_rate_kb", 0}}},
                            {"created_at", isoTime(created)}};
        j["api_token"] = withToken ? token : "";
        return j;
    }

    void Auth::load(const std::string &dir) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_path = dir + "/auth.json";
        m_users.clear();
        bool changed = false;
        json j = readJson(m_path);
        if (j.is_object() && j.contains("users") && j["users"].is_array()) {
            for (const auto &u : j["users"]) {
                if (!u.is_object()) {
                    continue;
                }
                User user;
                // the first version: name, password, token
                user.name = u.value("username", u.value("name", ""));
                user.id = u.value("id", user.name == kOwner ? kOwner : "");
                user.passwordHash = u.value("password_hash", "");
                if (user.passwordHash.empty() && !u.value("password", "").empty()) {
                    user.passwordHash = hashPassword(u.value("password", ""));
                    changed = true;
                }
                user.rank = u.value("rank", user.id == kOwner ? 100 : 10);
                user.token = u.value("api_token", u.value("token", ""));
                user.banned = u.value("is_banned", false);
                user.created = u.value("created_at", static_cast<int64_t>(0));
                user.preferences = u.value("preferences", json::object());
                if (!user.preferences.is_object()) user.preferences = json::object();
                if (user.name.empty() || (!user.id.empty() && findLocked(user.id))) {
                    continue;
                }
                if (user.id.empty()) {
                    user.id = "u_" + randomString(8);
                    changed = true;
                }
                user.rank = user.id == kOwner ? 100 : std::max(1, std::min(user.rank, 99));
                m_users.push_back(user);
            }
        }
        if (!findLocked(kOwner)) {
            User o;
            o.id = kOwner;
            o.name = kOwner;
            o.rank = 100;
            m_users.insert(m_users.begin(), o);
            changed = true;
        }
        for (User &u : m_users) {
            if (u.token.empty()) {
                u.token = newToken();
                changed = true;
            }
            if (u.created == 0) {
                u.created = static_cast<int64_t>(time(nullptr));
                changed = true;
            }
        }
        if (changed) {
            save();
        }
    }

    std::string Auth::setOwnerPassword(const std::string &password) {
        if (password.empty()) {
            return "owner password is empty";
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        User *o = findLocked(kOwner);
        if (!o) {
            return "owner account is missing";
        }
        // The password is host-configured. Preserve the stable owner ID, username,
        // API token, rank and all other user records.
        if (!checkPassword(o->passwordHash, password)) {
            o->passwordHash = hashPassword(password);
            save();
        }
        return "";
    }

    void Auth::save() const {
        json users = json::array();
        for (const User &u : m_users) {
            users.push_back({{"id", u.id},
                             {"username", u.name},
                             {"password_hash", u.passwordHash},
                             {"rank", u.rank},
                             {"api_token", u.token},
                             {"is_banned", u.banned},
                             {"created_at", u.created},
                             {"preferences", u.preferences}});
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

    User *Auth::findLocked(const std::string &id) {
        for (User &u : m_users) {
            if (u.id == id) {
                return &u;
            }
        }
        return nullptr;
    }

    User Auth::owner() const {
        for (const User &u : m_users) {
            if (u.id == kOwner) {
                return u;
            }
        }
        return User();
    }

    bool Auth::passwordSet() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !owner().passwordHash.empty();
    }

    User Auth::byToken(const HttpRequest &req) const {
        std::string token = req.param("token");
        const std::string header = req.header("authorization");
        if (token.empty() && header.compare(0, 7, "Bearer ") == 0) {
            token = header.substr(7);
        }
        if (token.empty()) token = cookie(req, "p2play_token");
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const User &u : m_users) {
            if (!token.empty() && !u.banned && equalTime(u.token, token)) {
                return u;
            }
        }
        return User();
    }

    User Auth::claimOwner(const std::string &password) {
        if (password.empty() || passwordSet()) {
            return User();
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        User *o = findLocked(kOwner);
        if (!o) {
            return User();
        }
        o->passwordHash = hashPassword(password);
        save();
        return *o;
    }

    User Auth::login(const std::string &name, const std::string &password) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const User &u : m_users) {
            if (u.name == name && !u.banned && checkPassword(u.passwordHash, password)) {
                return u;
            }
        }
        return User();
    }

    User Auth::find(const std::string &id) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const User &u : m_users) {
            if (u.id == id) {
                return u;
            }
        }
        return User();
    }

    nlohmann::json Auth::preferences(const std::string &id) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const User &user : m_users) {
            if (user.id == id) {
                const json values = user.preferences.is_object() ? user.preferences : json::object();
                const std::string language = values.value("language", std::string("en"));
                const std::string theme = values.value("theme", std::string("light"));
                return {{"language", language == "ru" ? "ru" : "en"},
                        {"theme", theme == "dark" || theme == "auto" ? theme : "light"}};
            }
        }
        return {{"language", "en"}, {"theme", "light"}};
    }

    std::string Auth::setPreferences(const std::string &id, const nlohmann::json &values) {
        if (!values.is_object() || values.empty()) return "preferences must be a non-empty object";
        for (auto it = values.begin(); it != values.end(); ++it) {
            if (it.key() == "language") {
                if (!it.value().is_string() || (it.value() != "en" && it.value() != "ru"))
                    return "language must be en or ru";
            } else if (it.key() == "theme") {
                if (!it.value().is_string() || (it.value() != "light" && it.value() != "dark" && it.value() != "auto"))
                    return "theme must be light, dark or auto";
            } else return "unknown preference: " + it.key();
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        for (User &user : m_users) {
            if (user.id == id) {
                if (!user.preferences.is_object()) user.preferences = json::object();
                for (auto it = values.begin(); it != values.end(); ++it) user.preferences[it.key()] = it.value();
                save();
                return "";
            }
        }
        return "user not found";
    }
    std::vector<User> Auth::list() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_users;
    }

    std::string Auth::create(const User &actor, const std::string &name, const std::string &password, int rank, User &out) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (name.empty() || password.empty()) {
            return "username and password are required";
        }
        if (actor.banned || rank < 1 || actor.rank <= rank) {
            return "permission denied";
        }
        for (const User &u : m_users) {
            if (u.name == name) {
                return "user already exists";
            }
        }
        User u;
        do {
            u.id = "u_" + randomString(8);
        } while (findLocked(u.id));
        u.name = name;
        u.passwordHash = hashPassword(password);
        u.rank = rank;
        u.token = newToken();
        u.created = static_cast<int64_t>(time(nullptr));
        m_users.push_back(u);
        save();
        out = u;
        return "";
    }

    std::string Auth::update(const User &actor, const std::string &id, const std::string *password, const bool *banned) {
        std::lock_guard<std::mutex> lock(m_mutex);
        User *target = findLocked(id);
        if (!target) {
            return "user not found";
        }
        const bool self = target->id == actor.id;
        // a password: one's own or of a lower rank; a ban: never the owner, nor oneself
        if (password && !self && actor.rank <= target->rank) {
            return "permission denied";
        }
        if (banned && (target->id == kOwner || self || actor.rank <= target->rank)) {
            return target->id == kOwner ? "cannot modify or delete the owner" : "permission denied";
        }
        if (password) {
            target->passwordHash = hashPassword(*password);
        }
        if (banned) {
            target->banned = *banned;
        }
        save();
        return "";
    }

    std::string Auth::remove(const User &actor, const std::string &id) {
        std::lock_guard<std::mutex> lock(m_mutex);
        User *target = findLocked(id);
        if (!target) {
            return "user not found";
        }
        if (target->id == kOwner) {
            return "cannot modify or delete the owner";
        }
        if (actor.banned || actor.rank <= target->rank) {
            return "permission denied";
        }
        m_users.erase(std::remove_if(m_users.begin(), m_users.end(), [&](const User &u) { return u.id == id; }),
                      m_users.end());
        save();
        return "";
    }

    std::string Auth::regenerateToken(const User &actor, const std::string &id, std::string &token) {
        std::lock_guard<std::mutex> lock(m_mutex);
        User *target = findLocked(id);
        if (!target) {
            return "user not found";
        }
        if (target->id != actor.id && actor.rank <= target->rank) {
            return "permission denied";
        }
        target->token = token = newToken();
        save();
        return "";
    }

    std::string Auth::setRank(const User &actor, const std::string &id, int rank) {
        std::lock_guard<std::mutex> lock(m_mutex);
        User *target = findLocked(id);
        if (!target) {
            return "user not found";
        }
        if (target->id == kOwner) {
            return "cannot modify or delete the owner";
        }
        if (rank < 1 || actor.rank <= rank || actor.rank <= target->rank) {
            return "permission denied";
        }
        target->rank = rank;
        save();
        return "";
    }

}
