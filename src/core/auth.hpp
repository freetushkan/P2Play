#pragma once

// The users, one set for every API: auth.json in the data directory. The schemes are those
// of the APIs P2Play is compatible with, each on its own routes:
//   /api (Silo's scheme): a lasting token a user, sent as ?token= (players),
//     "Authorization: Bearer" (clients) or the silo_token / p2play_token cookie (browsers);
//     POST /api/auth/login gives it for a name and a password.
//   the MatriX routes: "Authorization: Basic" (WWW-Authenticate).
// As Silo: the owner (id "owner", rank 100) always exists; without a password set for any
// user nothing is asked and every request is the owner's. Ranks: owner 100, admins 50-99,
// users 10-49, guests 1-9; a user manages those of a lower rank only.
// auth.json: {"users": [{"id", "username", "password_hash", "rank", "api_token", "is_banned",
// "created_at"}]}. A plain "password" (written by hand) is hashed at the start.

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

        bool valid() const { return !id.empty(); }

        // as Silo's /api/auth/me and /api/users (the hash never leaves; the token only for
        // the user himself)
        nlohmann::json json(bool withToken) const;
    };

    class Auth {
    public:
        static constexpr const char *kOwner = "owner";

        void load(const std::string &dir);

        // a password is set: requests need a user
        bool required() const;

        // the user of a token (?token=, Bearer, cookie) or of Basic; invalid if none. Without
        // users required both give the owner.
        User byToken(const HttpRequest &req) const;

        User byBasic(const HttpRequest &req) const;

        User find(const std::string &id) const;

        // the user and his token, invalid if the name or the password is wrong
        User login(const std::string &name, const std::string &password) const;

        std::vector<User> list() const;

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
