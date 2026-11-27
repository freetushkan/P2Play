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

    async function nativeRequest(path, method, body) {
        var opts = { method: method, credentials: "same-origin", cache: "no-store",
            headers: { Accept: "application/json" } };
        if (body !== undefined) {
            if (body instanceof FormData) opts.body = body;
            else { opts.headers["Content-Type"] = "application/json"; opts.body = JSON.stringify(body); }
        }
        var response = await fetch("/api/v1" + path, opts), data;
        try { data = await response.json(); } catch (_) { throw new Error("Unreadable library response."); }
        if (response.status === 401) { showLogin(); throw new Error("Log in to this P2Play node."); }
        if (!response.ok) throw new Error(data.error || "Library request failed.");
        return data;
    }
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
    if (!SILO) {
        host.library = function (q, category, sort, order) {
            return nativeRequest("/library/torrents?q=" + encodeURIComponent(q || "") +
                "&category=" + encodeURIComponent(category || "") + "&sort=" + encodeURIComponent(sort || "date") +
                "&order=" + encodeURIComponent(order || "desc"), "GET");
        };
        host.addLibrary = function (data) { return nativeRequest("/torrents", "POST", data); };
        host.updateLibrary = function (hash, category) {
            return nativeRequest("/torrents/" + encodeURIComponent(hash), "PUT", { category: category });
        };
        host.removeLibrary = function (hash) { return nativeRequest("/torrents/" + encodeURIComponent(hash), "DELETE"); };
    }
    function showLogin() {
        $("login-view").hidden = false;
        ["search", "lists", "settings", "library"].forEach(function (id) { $(id + "-view").hidden = true; });
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
                el("p", "Version " + l.seq + (l.records ? " / " + l.records + " releases" : ""), "hint"),
                el("p", l.type === "meta" ? "Meta-list · enabled entries are subscribed automatically" : "Catalog list", "hint"),
                el("code", l.pubkey));
            if (l.type === "meta" && Array.isArray(l.items)) {
                l.items.forEach(function (item) {
                    var label = item.enabled ? "Included" : "Disabled";
                    copy.append(el("p", label + " · " + (item.annotation || "No annotation") + " · " + item.pubkey, "hint"));
                });
            }
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
            ["search", "lists", "settings", "library"].forEach(function (id) { $(id + "-view").hidden = id !== view; });
            if (view === "lists") await loadLists();
            else if (view === "settings") { await loadSettings(); status("Connection settings loaded."); }
            else if (view === "library" && !SILO) await loadLibrary();
            else status("Search the catalogs subscribed on your daemon.");
        }); });
    });

    function refreshLibraryCategories(categories) {
        var select = $("library-category"), old = select.value;
        select.replaceChildren();
        [["", "All categories"], ["none", "Uncategorized"]].forEach(function (entry) {
            var option = el("option", entry[1]); option.value = entry[0]; select.append(option);
        });
        (categories || []).forEach(function (category) {
            var option = el("option", category); option.value = category; select.append(option);
        });
        if (Array.from(select.options).some(function (option) { return option.value === old; })) select.value = old;
    }
    function renderLibrary(items) {
        var target = $("library-rows"); target.replaceChildren();
        if (!items.length) { target.append(el("p", "Your library is empty. Add a magnet or .torrent file above.", "empty")); return; }
        items.forEach(function (item) {
            var row = el("article", undefined, "row library-row"), copy = el("div", undefined, "row-copy");
            copy.append(el("h3", item.title || item.hash),
                el("p", bytes(item.size) + " · " + (item.file_count || 0) + " files", "hint"),
                el("p", item.state_text || "Torrent", "hint"), el("code", item.hash));
            var label = el("label", "Category"), input = el("input");
            input.type = "text"; input.maxLength = 128; input.value = item.category || "";
            input.setAttribute("aria-label", "Category for " + (item.title || item.hash)); label.append(input); copy.append(label);
            var actions = el("div", undefined, "library-actions");
            actions.append(button("Save changes", async function (b) {
                b.disabled = true;
                try {
                    await host.updateLibrary(item.hash, input.value);
                    status("Category updated. Category metadata is shared by this infohash across users.");
                    await loadLibrary();
                } finally { b.disabled = false; }
            }));
            actions.append(button("Remove", async function (b) {
                if (!window.confirm("Remove this torrent from your library? Other users are not affected.")) return;
                b.disabled = true;
                try { await host.removeLibrary(item.hash); status("Removed from your library."); await loadLibrary(); }
                finally { b.disabled = false; }
            }));
            row.append(copy, actions); target.append(row);
        });
    }
    async function loadLibrary() {
        status("Loading your library…");
        var data = await host.library($("library-query").value, $("library-category").value,
            $("library-sort").value, $("library-order").value);
        refreshLibraryCategories(data.categories || []);
        renderLibrary(data.items || []);
        status((data.total || 0) + " torrent(s) in your filtered library.");
    }
    $("library-reload").addEventListener("click", function () { run(loadLibrary); });
    $("library-filter-form").addEventListener("submit", function (event) { event.preventDefault(); run(loadLibrary); });
    $("library-add-form").addEventListener("submit", function (event) {
        event.preventDefault(); run(async function () {
            var link = $("library-link").value.trim(), file = $("library-file").files[0];
            if (!!link === !!file) throw new Error("Choose exactly one magnet/link or one .torrent file.");
            if (file && !file.size) throw new Error("The selected torrent file is empty.");
            var form = new FormData();
            if (file) form.append("file", file, file.name); else form.append("link", link);
            form.append("title", $("library-title").value.trim());
            form.append("category", $("library-new-category").value.trim());
            status("Adding torrent to your library…");
            var result = await host.addLibrary(form);
            $("library-add-form").reset();
            status("Torrent added: " + ((result.torrent && result.torrent.title) || (result.torrent && result.torrent.hash) || "complete"));
            await loadLibrary();
        });
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
        $("library-tab").hidden = SILO;
        $("library-view").hidden = true;
        $("settings-tab").hidden = !SILO || !session.can_admin;
        $("subscribe-form").hidden = !session.can_admin;
        status(SILO ? "Search the catalogs subscribed on your daemon." :
            "Search MediaMesh catalogs or manage your personal torrent library.");
    }
    document.title = "MediaMesh · " + HOST;
    run(start);
}());
