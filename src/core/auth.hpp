#pragma once

// Who sends a request. Off until users are set (auth.json, or MatriX's accs.db): then
// every route but the public ones needs a user.
//   Tokens, as Silo (the native scheme): a lasting token a user, sent as ?token= (players),
//     "Authorization: Bearer" (clients) or the p2play_token / silo_token cookie (browsers);
//     POST /api/auth/login gives it for a name and a password.
//   Basic, as MatriX (P2P_COMPAT_MATRIX): "Authorization: Basic"; accs.db ({"name":
//     "password"}) is read as MatriX keeps it.
// auth.json: {"users": [{"name": "owner", "password": "...", "token": "ts_..."}]}; a user
// without a token gets one (written back).

#include <mutex>
#include <string>
#include <vector>

#include "http.hpp"

namespace p2p {

    class Auth {
    public:
        struct User {
            std::string name;
            std::string password;
            std::string token;
        };

        void load(const std::string &dir);

        bool required() const;

        // the user of a request, empty if none
        std::string identify(const HttpRequest &req) const;

        // the user's token, empty if the name or the password is wrong
        std::string login(const std::string &name, const std::string &password) const;

        std::string tokenOf(const std::string &name) const;

        bool basicEnabled() const;

    private:
        void save() const;

        std::string m_path;
        mutable std::mutex m_mutex;
        std::vector<User> m_users;
    };

}
