(function () {
    "use strict";
    // One page for two hosts: Silo serves it as this plugin (/plugins/mediamesh/, service.js
    // passes the calls on to a P2Play node) and the P2Play core serves the same files at /ui/
    // (cmake/embed-ui.cmake), calling its own /api. The host is told by the address.
    var SILO = location.pathname.indexOf("/plugins/") === 0;
    var HOST = SILO ? "Silo" : "P2Play";
    var base = "/plugins/mediamesh", session, busy = false;
    var $ = function (id) { return document.getElementById(id); };
    function status(message, bad) { $("status").textContent = message; $("status").className = bad ? "error" : ""; }
    async function request(url, body, headers) {
        var opts = { credentials: "same-origin", headers: { Accept: "application/json" }, cache: "no-store" };
        if (body !== undefined) {
            opts.method = "POST";
            opts.headers["Content-Type"] = "application/json";
            Object.keys(headers || {}).forEach(function (k) { opts.headers[k] = headers[k]; });
            opts.body = JSON.stringify(body);
        }
        var res = await fetch(url, opts), data;
        try { data = await res.json(); } catch (_) { throw new Error(HOST + " returned an unreadable response. Sign in and reload this page."); }
        if (res.status === 401 && !SILO) { showLogin(); throw new Error("Log in to this P2Play node."); }
        if (!res.ok) throw new Error(data.error || "Request failed.");
        return data;
    }
    // the plugin's routes (service.js)
    function api(path, body) { return request(base + path, body, session ? { "X-MediaMesh-CSRF": session.csrf } : {}); }
    // the core's own API, in the shapes of the plugin's routes
    function core(path, body) { return request("/api" + path, body); }
    function card(r) {
        return { hash: r.hash, title: r.title || "", size: r.size || 0, files: r.files || 0, tags: r.tags || [],
            lists: r.lists || [], torrs: r.torrs || "", supported: true };
    }
    var host = SILO ? {
        session: function () { return api("/api/session"); },
        search: function (q, tag) { return api("/api/search?q=" + encodeURIComponent(q) + "&tag=" + encodeURIComponent(tag)); },
        lists: function () { return api("/api/lists"); },
        discover: function () { return api("/api/discover"); },
        subscribe: function (pubkey) { return api("/api/subscribe", { pubkey: pubkey }); },
        add: function (r) { return api("/api/add", { hash: r.hash }); },
        settings: function (body) { return api("/api/settings", body); }
    } : {
        session: async function () {
            var me = await core("/auth/me");
            return { rank: me.rank, csrf: "", can_admin: me.rank >= 50, user: me.username };
        },
        search: async function (q, tag) {
            var data = await core("/catalog/search?q=" + encodeURIComponent(q) + "&tag=" + encodeURIComponent(tag) + "&limit=50");
            return { results: (data.results || []).map(card) };
        },
        lists: function () { return core("/lists"); },
        discover: function () { return core("/lists/discover"); },
        subscribe: function (pubkey) { return core("/lists/" + encodeURIComponent(pubkey.toLowerCase()), {}); },
        add: function (r) {
            // a v1 record has its torrs:// card (title, poster, trackers); a v2 one goes by its hash
            var link = r.torrs || (r.hash.length === 64 ? "magnet:?xt=urn:btmh:1220" + r.hash : "magnet:?xt=urn:btih:" + r.hash);
            return core("/torrents", { link: link, title: r.title, save_to_db: true });
        }
    };
    function showLogin() {
        $("login-view").hidden = false;
        ["search", "lists", "settings"].forEach(function (id) { $(id + "-view").hidden = true; });
    }
    async function run(action) {
        if (busy) return;
        busy = true; document.body.setAttribute("aria-busy", "true");
        try { await action(); } catch (err) { status(err.message || "Request failed.", true); }
        finally { busy = false; document.body.removeAttribute("aria-busy"); }
    }
    function el(tag, value, className) {
        var node = document.createElement(tag);
        if (value !== undefined) node.textContent = value;
        if (className) node.className = className;
        return node;
    }
    function button(label, action) {
        var b = el("button", label); b.type = "button";
        b.addEventListener("click", function () { run(function () { return action(b); }); });
        return b;
    }
    function bytes(n) {
        if (!Number.isFinite(Number(n)) || n <= 0) return "Size unknown";
        var units = ["B", "KiB", "MiB", "GiB", "TiB"], i = 0;
        while (n >= 1024 && i < units.length - 1) { n /= 1024; i++; }
        return n.toFixed(i ? 1 : 0) + " " + units[i];
    }
    function renderResults(results) {
        var target = $("results"); target.replaceChildren();
        if (!results.length) target.append(el("p", "No releases found. Try another title or subscribe to a list.", "empty"));
        results.forEach(function (r) {
            var row = el("article", undefined, "row"), copy = el("div", undefined, "row-copy");
            copy.append(el("h3", r.title || r.hash), el("p", bytes(r.size) + (r.files ? " / " + r.files + " files" : "")), el("p", r.tags.join(", "), "hint"));
            var hash = el("code", r.hash); copy.append(hash); row.append(copy);
            var add = button("Add to " + HOST, async function (b) {
                b.disabled = true; status("Adding to your " + HOST + " library…");
                try { await host.add(r); b.textContent = "Added"; status("Added to your " + HOST + " library."); }
                catch (err) { b.disabled = false; throw err; }
            });
            if (!r.supported) { add.disabled = true; add.textContent = "v2 not supported"; }
            row.append(add); target.append(row);
        });
    }
    function renderLists(target, lists, discovery) {
        target.replaceChildren();
        if (!lists.length) target.append(el("p", discovery ? "No lists advertised yet. Check the daemon's network connection or enter a public key below." : "No subscribed lists. Discover a list below.", "empty"));
        lists.forEach(function (l) {
            var row = el("article", undefined, "row"), copy = el("div", undefined, "row-copy");
            copy.append(el("h3", l.name || "Unnamed list"), el("p", l.description || "No description"),
                el("p", "Version " + l.seq + (l.records ? " / " + l.records + " releases" : ""), "hint"), el("code", l.pubkey));
            row.append(copy);
            if (session.can_admin) row.append(button(discovery ? "Subscribe" : "Request refresh", async function () {
                await host.subscribe(l.pubkey); status("Subscription requested. Reload lists after the daemon downloads the signed catalog.");
            }));
            target.append(row);
        });
    }
    async function loadLists() {
        status("Loading lists…");
        var own = await host.lists(); renderLists($("lists"), own.lists, false);
        // Keep subscribed lists visible if discovery is temporarily unavailable.
        var discovered = await host.discover(); renderLists($("discover"), discovered.lists, true);
        status("Lists loaded.");
    }
    async function loadSettings() {
        var cfg = await host.settings(); $("daemon-url").value = cfg.url;
        $("daemon-token").value = ""; $("clear-token").checked = false;
        $("token-state").textContent = cfg.token_configured ? "A token is stored on the Silo server. It is never returned to this page." : "No daemon token is stored.";
    }
    document.querySelectorAll("[data-view]").forEach(function (b) {
        b.addEventListener("click", function () { run(async function () {
            if (!session) return;
            var view = b.getAttribute("data-view");
            document.querySelectorAll("[data-view]").forEach(function (tab) { tab.setAttribute("aria-pressed", String(tab === b)); });
            ["search", "lists", "settings"].forEach(function (id) { $(id + "-view").hidden = id !== view; });
            if (view === "lists") await loadLists();
            else if (view === "settings") { await loadSettings(); status("Connection settings loaded."); }
            else status("Search the catalogs subscribed on your daemon.");
        }); });
    });
    $("search-form").addEventListener("submit", function (e) { e.preventDefault(); run(async function () {
        status("Searching…");
        var data = await host.search($("query").value, $("tag").value);
        renderResults(data.results); status(data.results.length + " releases found. Showing up to 50 results.");
    }); });
    $("reload-lists").addEventListener("click", function () { run(loadLists); });
    $("subscribe-form").addEventListener("submit", function (e) { e.preventDefault(); run(async function () {
        await host.subscribe($("pubkey").value.trim());
        status("Subscription requested. Reload lists after the catalog downloads.");
    }); });
    $("settings-form").addEventListener("submit", function (e) { e.preventDefault(); run(async function () {
        var token = $("daemon-token").value;
        $("daemon-token").value = "";
        await host.settings({ url: $("daemon-url").value.trim(), token: token, clear_token: $("clear-token").checked });
        await loadSettings(); status("Connection saved. Use Search or Lists to check the daemon.");
    }); });
    $("login-form").addEventListener("submit", function (e) { e.preventDefault(); run(async function () {
        // the core's login: its token comes back as a cookie (the first login as owner sets the password)
        await request("/api/auth/login", { username: $("username").value.trim(), password: $("password").value });
        $("password").value = ""; $("login-view").hidden = true;
        await start();
    }); });
    async function start() {
        session = await host.session();
        document.querySelectorAll(".silo-only").forEach(function (n) { n.hidden = !SILO; });
        $("search-view").hidden = false;
        $("settings-tab").hidden = !SILO || !session.can_admin;
        $("subscribe-form").hidden = !session.can_admin;
        status("Search the catalogs subscribed on " + (SILO ? "your daemon." : "this node."));
    }
    document.title = "MediaMesh · " + HOST;
    run(start);
}());
