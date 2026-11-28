#pragma once

// P2Play accounts and durable API keys, stored in auth.json.
// Each user has one stable API token. Login returns that existing token; it is not rotated
// until the user (or an administrator with sufficient rank) explicitly resets it.
// Requests use Authorization: Bearer, the ?token= query for shareable stream URLs, or the
// HttpOnly p2play_token cookie used by the web UI. The owner (id "owner", rank 100) always
// exists; the first owner login may set its password when no password has been configured.
// auth.json stores password hashes and one api_token per user. Legacy plaintext passwords
// are hashed on load.

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "../../third_party/nlohmann/json.hpp"
#include "http.hpp"

namespace p2p {

    struct User {
        std::string id;
        std::string name;
        std::string passwordHash;   // sha256$<rounds>$<salt>$<hash>, empty: no password
        int rank = 10;
        std::string token;
        bool banned = false;
        int64_t created = 0;
        nlohmann::json preferences = nlohmann::json::object();

        bool valid() const { return !id.empty(); }

        // Public account representation. API tokens are returned only from login,
        // user creation/reset and explicit stream-link creation.
        nlohmann::json json(bool withToken) const;
    };

    class Auth {
    public:
        static constexpr const char *kOwner = "owner";

        void load(const std::string &dir);
        // Embedded host: update only the fixed owner's password, preserving ID and token.
        std::string setOwnerPassword(const std::string &password);

        // the owner has a password (else the first login sets it)
        bool passwordSet() const;

        // the owner without a password (none set yet): this one set; the owner, else invalid
        User claimOwner(const std::string &password);

        // Resolve the stable user's token from a share URL, Bearer header or web cookie.
        User byToken(const HttpRequest &req) const;

        User find(const std::string &id) const;

        // the user and his token, invalid if the name or the password is wrong
        User login(const std::string &name, const std::string &password) const;

        std::vector<User> list() const;
        nlohmann::json preferences(const std::string &id) const;
        std::string setPreferences(const std::string &id, const nlohmann::json &values);

        // the user management of Silo; an empty string: done, else the error
        std::string create(const User &actor, const std::string &name, const std::string &password, int rank, User &out);

        std::string update(const User &actor, const std::string &id, const std::string *password, const bool *banned);

        std::string remove(const User &actor, const std::string &id);

        std::string regenerateToken(const User &actor, const std::string &id, std::string &token);

        std::string setRank(const User &actor, const std::string &id, int rank);

    private:
        User owner() const;

        User *findLocked(const std::string &id);

        void save() const;

        std::string m_path;
        mutable std::mutex m_mutex;
        std::vector<User> m_users;
    };

}
