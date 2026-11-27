/* Silo host contracts pinned in docs/silo-plugin.md. No Node/browser APIs here. */
(function () {
    "use strict";
    var DEFAULT_URL = "http://127.0.0.1:8080";
    var csrfSecret = ts.crypto.randomBytes(32);
    var HASH = /^[0-9a-f]{40}$/i;
    var PUBKEY = /^[0-9a-f]{64}$/i;

    function fail(code, message) { throw { meshCode: code, meshMessage: message }; }
    function text(value, max) { return typeof value === "string" ? value.slice(0, max) : ""; }
    function settings() {
        var raw = ts.storage.get("settings");
        return raw ? JSON.parse(raw) : { url: DEFAULT_URL, token: "" };
    }
    function csrf(user) { return ts.crypto.hmacSha256(csrfSecret, "mediamesh:" + user.id); }
    function headers(res) {
        res.header("Cache-Control", "no-store").header("X-Content-Type-Options", "nosniff")
            .header("Referrer-Policy", "no-referrer");
    }
    function route(method, path, minRank, handler) {
        ts.web[method](path, function (req, res) {
            headers(res);
            try {
                if (!req.user || req.user.is_banned) fail(401, "Sign in to Silo first.");
                if (req.user.rank < minRank) fail(403, "Insufficient Silo permissions.");
                if (method === "post") {
                    if (text(req.headers["content-type"], 128).split(";")[0].trim().toLowerCase() !== "application/json")
                        fail(415, "Use an application/json request.");
                    if (!ts.crypto.constantTimeCompare(req.headers["x-mediamesh-csrf"] || "", csrf(req.user)) || req.headers["sec-fetch-site"] === "cross-site")
                        fail(403, "Reload MediaMesh before making changes.");
                    if (!req.body || typeof req.body !== "object" || Array.isArray(req.body) || req.raw.length > 16384)
                        fail(400, "Invalid request body.");
                }
                handler(req, res);
            } catch (err) {
                // Never echo upstream exception text, response bodies, URLs or credentials.
                res.status(err && err.meshCode || 502).json({ error: err && err.meshMessage || "MediaMesh operation failed. Check the daemon and its credentials." });
            }
        });
    }
    function daemon(method, path, body) {
        var cfg = settings();
        var opts = { timeout: 4000, headers: { Accept: "application/json" } };
        if (cfg.token) opts.headers.Authorization = "Bearer " + cfg.token;
        var response;
        if (method === "post") {
            opts.headers["Content-Type"] = "application/json";
            response = ts.http.post(cfg.url + path, body, opts);
        } else response = ts.http.get(cfg.url + path, opts);
        if (!response.ok) {
            if (response.status === 404) fail(404, "Not found in the daemon catalog.");
            fail(502, "Daemon returned HTTP " + response.status + ". Check its configuration and permissions.");
        }
        if (response.body.length > 4 * 1024 * 1024) fail(502, "Daemon response is too large.");
        var data = response.json();
        if (!data || typeof data !== "object") fail(502, "Invalid daemon response.");
        return data;
    }
    function daemonURL(input) {
        if (typeof input !== "string" || input.length > 256) fail(400, "Invalid daemon URL.");
        // Numeric addresses / localhost only: no DNS rebinding, userinfo, path, query or fragment.
        var m = /^(https?):\/\/(localhost|[0-9.]+|\[[0-9a-f:]+\])(?::([0-9]{1,5}))?\/?$/i.exec(input);
        if (!m) fail(400, "Use http(s)://localhost or a numeric IP address, with an optional port.");
        var host = m[2].toLowerCase();
        if (m[3] && (+m[3] < 1 || +m[3] > 65535)) fail(400, "Invalid daemon port.");
        if (host[0] === "[") {
            var ip = host.slice(1, -1), halves = ip.split("::"), groups = ip.replace(/^:|:$/g, "").split(":");
            var count = 0;
            for (var j = 0; j < groups.length; j++) {
                if (!groups[j]) continue;
                if (!/^[0-9a-f]{1,4}$/.test(groups[j])) fail(400, "Invalid IPv6 address.");
                count++;
            }
            if (halves.length > 2 || (halves.length === 1 && count !== 8) || (halves.length === 2 && count >= 8) ||
                (ip[0] === ":" && ip.slice(0, 2) !== "::") || (ip[ip.length - 1] === ":" && ip.slice(-2) !== "::") || ip.indexOf(":::") >= 0)
                fail(400, "Invalid IPv6 address.");
        } else if (host !== "localhost") {
            var octets = host.split(".");
            if (octets.length !== 4) fail(400, "Invalid IPv4 address.");
            for (var i = 0; i < 4; i++) if (!/^(0|[1-9][0-9]{0,2})$/.test(octets[i]) || +octets[i] > 255)
                fail(400, "Invalid IPv4 address.");
        }
        return m[1].toLowerCase() + "://" + host + (m[3] ? ":" + (+m[3]) : "");
    }
    function record(hash) {
        if (!HASH.test(hash)) fail(400, "Select a v1 (40-character) torrent hash.");
        var r = daemon("get", "/api/catalog/" + hash.toLowerCase());
        if (!HASH.test(r.hash) || r.hash.toLowerCase() !== hash.toLowerCase()) fail(502, "Daemon returned the wrong torrent.");
        return r;
    }
    function publicRecord(r) {
        // Explicit projection: no raw upstream fields, external HTML or images in the browser.
        return { hash: text(r.hash, 64), title: text(r.title, 1024), size: r.size || 0,
            files: r.files || 0, tags: Array.isArray(r.tags) ? r.tags.slice(0, 64).map(function (x) { return text(x, 512); }) : [],
            lists: Array.isArray(r.lists) ? r.lists.filter(function (x) { return PUBKEY.test(x); }).slice(0, 128) : [],
            supported: HASH.test(r.hash) };
    }
    function publicLists(data) {
        if (!Array.isArray(data.lists)) fail(502, "Invalid daemon list response.");
        return { lists: data.lists.slice(0, 1000).map(function (l) {
            var items = Array.isArray(l.items) ? l.items.slice(0, 256).map(function (item) {
                return { pubkey: text(item.pubkey, 64), annotation: text(item.annotation, 2048),
                    enabled: item.enabled === true };
            }).filter(function (item) { return PUBKEY.test(item.pubkey); }) : [];
            return { pubkey: text(l.pubkey, 64), name: text(l.name, 512), description: text(l.description, 2048),
                seq: l.seq || 0, records: l.records || 0, own: l.own === true, holders: l.holders || 0,
                type: l.type === "meta" ? "meta" : "catalog", items: items };
        }).filter(function (l) { return PUBKEY.test(l.pubkey); }) };
    }
    route("get", "/", 10, function (req, res) {
        res.header("Content-Security-Policy", "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'")
            .html(ts.vfs.read("index.html"));
    });
    route("get", "/index.html", 10, function (req, res) {
        res.header("Content-Security-Policy", "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'")
            .html(ts.vfs.read("index.html"));
    });
    ts.web.staticFile("/app.js", "app.js");
    ts.web.staticFile("/style.css", "style.css");
    ts.web.staticFile("/lang/en.json", "lang/en.json");
    ts.web.staticFile("/lang/ru.json", "lang/ru.json");
    route("get", "/api/session", 10, function (req, res) {
        res.json({ rank: req.user.rank, csrf: csrf(req.user), can_admin: req.user.rank >= 50,
            capabilities: { subscribe: true, remove: false, refresh: "resubscribe", v1_only: true } });
    });
    route("get", "/api/settings", 50, function (req, res) {
        var cfg = settings();
        res.json({ url: cfg.url, token_configured: !!cfg.token });
    });
    route("post", "/api/settings", 50, function (req, res) {
        var current = settings(), b = req.body;
        var url = daemonURL(b.url);
        if (typeof b.token !== "undefined" && (typeof b.token !== "string" || b.token.length > 512 || /[\x00-\x20\x7f]/.test(b.token)))
            fail(400, "Token must contain no whitespace or control characters.");
        var token = b.clear_token === true ? "" : (b.token || current.token);
        ts.storage.set("settings", JSON.stringify({ url: url, token: token }));
        res.json({ status: "saved", token_configured: !!token });
    });
    route("get", "/api/search", 10, function (req, res) {
        var q = text(req.query.q, 512), tag = text(req.query.tag, 128);
        var limit = Math.min(100, Math.max(1, parseInt(req.query.limit, 10) || 50));
        var data = daemon("get", "/api/catalog/search?q=" + encodeURIComponent(q) + "&tag=" + encodeURIComponent(tag) + "&limit=" + limit);
        if (!Array.isArray(data.results)) fail(502, "Invalid daemon search response.");
        res.json({ results: data.results.slice(0, limit).map(publicRecord) });
    });
    route("get", "/api/record", 10, function (req, res) { res.json(publicRecord(record(text(req.query.hash, 64)))); });
    route("get", "/api/lists", 10, function (req, res) { res.json(publicLists(daemon("get", "/api/lists"))); });
    route("get", "/api/discover", 10, function (req, res) { res.json(publicLists(daemon("get", "/api/lists/discover"))); });
    route("post", "/api/subscribe", 50, function (req, res) {
        var key = req.body.pubkey;
        if (!PUBKEY.test(key)) fail(400, "A 64-character list public key is required.");
        daemon("post", "/api/lists/" + key.toLowerCase(), {});
        res.status(202).json({ status: "subscribing" });
    });
    route("post", "/api/add", 10, function (req, res) {
        var r = record(text(req.body.hash, 64));
        // Silo ParseTorrentSpec accepts magnet or v1 hash, NOT torrs://. Resolve the
        // selected catalog card server-side, preserving its title and trackers.
        var magnet = "magnet:?xt=urn:btih:" + r.hash.toLowerCase() + "&dn=" + encodeURIComponent(text(r.title, 1024));
        if (Array.isArray(r.trackers)) r.trackers.slice(0, 64).forEach(function (tracker) {
            if (typeof tracker === "string" && tracker.length <= 512 && /^(https?|udp):\/\//i.test(tracker) && !/[\x00-\x20\x7f]/.test(tracker))
                magnet += "&tr=" + encodeURIComponent(tracker);
        });
        try {
            ts.torrent.add(magnet, { title: text(r.title, 1024), category: Array.isArray(r.tags) ? text(r.tags[0], 128) : "",
                user_id: req.user.id, save_to_db: true });
        } catch (_) { fail(502, "Silo could not add this torrent. Check your torrent quota and server logs."); }
        res.status(201).json({ status: "added", hash: r.hash.toLowerCase() });
    });
}());
