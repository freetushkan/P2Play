(function () {
    "use strict";
    var base = "/plugins/mediamesh", session, busy = false;
    var $ = function (id) { return document.getElementById(id); };
    function status(message, bad) { $("status").textContent = message; $("status").className = bad ? "error" : ""; }
    async function api(path, body) {
        var opts = { credentials: "same-origin", headers: { Accept: "application/json" }, cache: "no-store" };
        if (body !== undefined) {
            opts.method = "POST";
            opts.headers["Content-Type"] = "application/json";
            opts.headers["X-MediaMesh-CSRF"] = session.csrf;
            opts.body = JSON.stringify(body);
        }
        var res = await fetch(base + path, opts), data;
        try { data = await res.json(); } catch (_) { throw new Error("Silo returned an unreadable response. Sign in and reload this page."); }
        if (!res.ok) throw new Error(data.error || "Request failed.");
        return data;
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
            var add = button("Add to Silo", async function (b) {
                b.disabled = true; status("Adding to your Silo library…");
                try { await api("/api/add", { hash: r.hash }); b.textContent = "Added"; status("Added to your Silo library."); }
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
                await api("/api/subscribe", { pubkey: l.pubkey }); status("Subscription requested. Reload lists after the daemon downloads the signed catalog.");
            }));
            target.append(row);
        });
    }
    async function loadLists() {
        status("Loading lists…");
        var own = await api("/api/lists"); renderLists($("lists"), own.lists, false);
        // Keep subscribed lists visible if discovery is temporarily unavailable.
        var discovered = await api("/api/discover"); renderLists($("discover"), discovered.lists, true);
        status("Lists loaded.");
    }
    async function loadSettings() {
        var cfg = await api("/api/settings"); $("daemon-url").value = cfg.url;
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
        var data = await api("/api/search?q=" + encodeURIComponent($("query").value) + "&tag=" + encodeURIComponent($("tag").value));
        renderResults(data.results); status(data.results.length + " releases found. Showing up to 50 results.");
    }); });
    $("reload-lists").addEventListener("click", function () { run(loadLists); });
    $("subscribe-form").addEventListener("submit", function (e) { e.preventDefault(); run(async function () {
        await api("/api/subscribe", { pubkey: $("pubkey").value.trim() });
        status("Subscription requested. Reload lists after the catalog downloads.");
    }); });
    $("settings-form").addEventListener("submit", function (e) { e.preventDefault(); run(async function () {
        var token = $("daemon-token").value;
        $("daemon-token").value = "";
        await api("/api/settings", { url: $("daemon-url").value.trim(), token: token, clear_token: $("clear-token").checked });
        await loadSettings(); status("Connection saved. Use Search or Lists to check the daemon.");
    }); });
    run(async function () {
        session = await api("/api/session");
        $("settings-tab").hidden = !session.can_admin;
        $("subscribe-form").hidden = !session.can_admin;
        status("Search the catalogs subscribed on your daemon.");
    });
}());
