// Copied by silo-plugin-test.py into the pinned upstream internal/plugin package.
package plugin

import (
	"archive/zip"
	"bytes"
	"encoding/json"
	"fmt"
	"io/fs"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	"silo/internal/config"
	"silo/internal/database"
	"silo/internal/torrent"
	"silo/internal/user"

	"github.com/dop251/goja"
	"github.com/gin-gonic/gin"
)

const meshHash = "0123456789abcdef0123456789abcdef01234567"
const meshKey = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"
const meshToken = "test-daemon-secret-never-return"

// Real upstream registrar interface; handlers themselves are upstream ginHandler.
type meshRegistrar struct {
	handlers map[string]gin.HandlerFunc
	files    map[string]string
}

func (s *meshRegistrar) Reset() {
	s.handlers = map[string]gin.HandlerFunc{}
	s.files = map[string]string{}
}
func (s *meshRegistrar) SetActiveTheme(string) {}
func (s *meshRegistrar) ActiveThemeID() string { return "" }
func (s *meshRegistrar) RemovePlugin(string)   {}
func (s *meshRegistrar) AddJSHandler(id, method, route string, h gin.HandlerFunc) {
	if id == "mediamesh" {
		s.handlers[method+" "+route] = h
	}
}
func (s *meshRegistrar) AddStaticFile(id, route, file string, _ fs.FS) {
	if id == "mediamesh" {
		s.files[route] = file
	}
}
func (s *meshRegistrar) AddStaticDir(string, string, string, fs.FS) {}

type meshHarness struct {
	t                     *testing.T
	rt                    *JSRuntime
	reg                   *meshRegistrar
	admin, regular, guest string
	users                 *user.Service
	db                    *database.DB
	manifest              *Manifest
}

func meshNew(t *testing.T) *meshHarness {
	t.Helper()
	gin.SetMode(gin.TestMode)
	root := os.Getenv("MEDIAMESH_PLUGIN_DIR")
	if root == "" {
		t.Fatal("MEDIAMESH_PLUGIN_DIR is required")
	}
	vfs := os.DirFS(root)
	m, err := LoadManifestFromFS(vfs)
	if err != nil {
		t.Fatal(err)
	}
	db, err := database.Open(filepath.Join(t.TempDir(), "silo.db"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	cfg := config.DefaultConfig()
	cfg.Auth.OwnerPassword = "test-owner-password"
	svc := user.NewService(user.NewStore(db), cfg)
	owner, token, err := svc.Login("owner", cfg.Auth.OwnerPassword)
	if err != nil {
		t.Fatal(err)
	}
	regular, err := svc.CreateUser(owner, "mesh-user", "test-password", user.RankUser, user.Limits{})
	if err != nil {
		t.Fatal(err)
	}
	guest, err := svc.CreateUser(owner, "mesh-guest", "test-password", user.RankGuest, user.Limits{})
	if err != nil {
		t.Fatal(err)
	}
	reg := &meshRegistrar{}
	reg.Reset()
	rt := NewJSRuntime(m.ID, m, vfs, reg, db, nil, svc, NewI18nRegistry(), nil, nil, nil, nil)
	t.Cleanup(rt.Stop)
	code, err := fs.ReadFile(vfs, m.Entry)
	if err != nil {
		t.Fatal(err)
	}
	if err = rt.Execute(string(code)); err != nil {
		t.Fatal(err)
	}
	return &meshHarness{t, rt, reg, token, regular.APIToken, guest.APIToken, svc, db, m}
}
func (h *meshHarness) request(method, path, token string, body any, csrf string, extra ...map[string]string) *httptest.ResponseRecorder {
	h.t.Helper()
	var buf bytes.Buffer
	if body != nil {
		if err := json.NewEncoder(&buf).Encode(body); err != nil {
			h.t.Fatal(err)
		}
	}
	req := httptest.NewRequest(method, path, &buf)
	if token != "" {
		req.AddCookie(&http.Cookie{Name: "silo_token", Value: token})
	}
	if body != nil {
		req.Header.Set("Content-Type", "application/json; charset=utf-8")
	}
	if csrf != "" {
		req.Header.Set("X-MediaMesh-CSRF", csrf)
	}
	for _, headers := range extra {
		for name, value := range headers {
			req.Header.Set(name, value)
		}
	}
	recorder := httptest.NewRecorder()
	c, _ := gin.CreateTestContext(recorder)
	c.Request = req
	handler := h.reg.handlers[method+" "+req.URL.Path]
	if handler == nil {
		h.t.Fatalf("unregistered route %s %s", method, req.URL.Path)
	}
	handler(c)
	if strings.Contains(recorder.Body.String(), meshToken) {
		h.t.Fatal("daemon credential leaked")
	}
	if recorder.Header().Get("Cache-Control") != "no-store" {
		h.t.Fatal("missing no-store")
	}
	return recorder
}
func (h *meshHarness) csrf(token string) string {
	h.t.Helper()
	r := h.request("GET", "/api/session", token, nil, "")
	if r.Code != 200 {
		h.t.Fatalf("session: %s", r.Body)
	}
	var data struct {
		CSRF string `json:"csrf"`
	}
	if err := json.Unmarshal(r.Body.Bytes(), &data); err != nil {
		h.t.Fatal(err)
	}
	return data.CSRF
}
func (h *meshHarness) configure(url string) {
	h.t.Helper()
	r := h.request("POST", "/api/settings", h.admin, map[string]any{"url": url, "token": meshToken}, h.csrf(h.admin))
	if r.Code != 200 {
		h.t.Fatalf("configure: %d %s", r.Code, r.Body)
	}
}
func meshExpect(t *testing.T, r *httptest.ResponseRecorder, code int) {
	t.Helper()
	if r.Code != code {
		t.Fatalf("want %d, got %d: %s", code, r.Code, r.Body)
	}
}

func TestMediaMeshRealRuntimeAuthAndCSRF(t *testing.T) {
	h := meshNew(t)
	for _, path := range []string{"/", "/index.html", "/api/session", "/api/search", "/api/lists", "/api/discover", "/api/record", "/api/settings"} {
		meshExpect(t, h.request("GET", path, "", nil, ""), 401)
		meshExpect(t, h.request("GET", path, h.guest, nil, ""), 403)
	}
	for _, path := range []string{"/api/settings", "/api/subscribe", "/api/add"} {
		meshExpect(t, h.request("POST", path, h.admin, map[string]any{}, ""), 403)
		meshExpect(t, h.request("POST", path, "", map[string]any{}, ""), 401)
	}
	meshExpect(t, h.request("GET", "/api/settings", h.regular, nil, ""), 403)
	meshExpect(t, h.request("POST", "/api/settings", h.regular, map[string]any{}, h.csrf(h.regular)), 403)
	meshExpect(t, h.request("POST", "/api/subscribe", h.regular, map[string]any{}, h.csrf(h.regular)), 403)
	meshExpect(t, h.request("POST", "/api/settings", h.admin, map[string]any{}, h.csrf(h.regular)), 403)
	meshExpect(t, h.request("POST", "/api/settings", h.admin, nil, h.csrf(h.admin)), 415)

	meshExpect(t, h.request("POST", "/api/settings", h.admin, map[string]any{"url": "http://127.0.0.1"}, h.csrf(h.admin), map[string]string{"Sec-Fetch-Site": "cross-site"}), 403)
	meshExpect(t, h.request("POST", "/api/settings", h.admin, map[string]any{"url": "http://127.0.0.1"}, h.csrf(h.admin), map[string]string{"Content-Type": "application/x-www-form-urlencoded"}), 415)
	meshExpect(t, h.request("POST", "/api/settings", h.admin, []any{}, h.csrf(h.admin)), 400)
	meshExpect(t, h.request("POST", "/api/settings", h.admin, map[string]any{"url": "http://127.0.0.1", "token": strings.Repeat("a", 17000)}, h.csrf(h.admin)), 400)
	root := h.request("GET", "/", h.regular, nil, "")
	meshExpect(t, root, 200)
	if !strings.Contains(root.Header().Get("Content-Security-Policy"), "frame-ancestors 'none'") {
		t.Fatal("missing CSP")
	}
	if !strings.Contains(root.Body.String(), "Search your catalogs") {
		t.Fatal("real VFS page not served")
	}
	for key := range h.reg.handlers {
		route := strings.SplitN(key, " ", 2)[1]
		if !routeAllowed(h.manifest, route) {
			t.Errorf("route not whitelisted: %s", route)
		}
	}
	if len(h.reg.handlers) != 11 || len(h.reg.files) != 4 {
		t.Fatalf("route inventory: %v %v", h.reg.handlers, h.reg.files)
	}
	for _, path := range []string{"/app.js", "/style.css", "/lang/en.json", "/lang/ru.json"} {
		if _, ok := h.reg.files[path]; !ok {
			t.Errorf("missing static UI asset route %s", path)
		}
	}
	siloSession := h.request("GET", "/api/session", h.regular, nil, "")
	var sessionData map[string]any
	if err := json.Unmarshal(siloSession.Body.Bytes(), &sessionData); err != nil {
		t.Fatal(err)
	}
	siloUser, err := h.users.Authenticate(h.regular)
	if err != nil {
		t.Fatal(err)
	}
	if sessionData["user"] != siloUser.ID {
		t.Fatalf("session user id: got %v, want %v", sessionData["user"], siloUser.ID)
	}
	owner, err := h.users.GetUserByID("owner")
	if err != nil {
		t.Fatal(err)
	}
	regular, err := h.users.Authenticate(h.regular)
	if err != nil {
		t.Fatal(err)
	}
	if err := h.users.BanUser(owner, regular.ID, true); err != nil {
		t.Fatal(err)
	}
	meshExpect(t, h.request("GET", "/api/session", h.regular, nil, ""), 401)

}

func TestMediaMeshSettingsValidationAndPersistence(t *testing.T) {
	h := meshNew(t)
	csrf := h.csrf(h.admin)
	for _, url := range []string{"http://example.com:8080", "http://user:pass@127.0.0.1", "http://127.0.0.1/api", "http://127.0.0.1?token=x", "http://127.0.0.1#x", "file:///tmp/a", "http://2130706433", "http://127.00.0.1", "http://999.0.0.1", "http://[:::1]", "http://[1:2:3]", "http://127.0.0.1:65536", "http://127.0.0.1:0", "http://127.0.0.1\r\nHeader:x"} {
		meshExpect(t, h.request("POST", "/api/settings", h.admin, map[string]any{"url": url}, csrf), 400)
	}
	for _, url := range []string{"http://127.0.0.1:8080/", "https://192.168.1.10:8443", "http://localhost", "http://[::1]:8080", "http://[2001:db8:0:0:0:0:0:1]"} {
		meshExpect(t, h.request("POST", "/api/settings", h.admin, map[string]any{"url": url}, csrf), 200)
	}
	h.configure("http://127.0.0.1:8080")
	meshExpect(t, h.request("POST", "/api/settings", h.admin, map[string]any{"url": "http://127.0.0.1:8080", "token": ""}, csrf), 200)
	if !strings.Contains(h.request("GET", "/api/settings", h.admin, nil, "").Body.String(), `"token_configured":true`) {
		t.Fatal("blank did not preserve token")
	}
	var cfg struct{ Token string }
	raw := h.rt.vm.Get("ts").ToObject(h.rt.vm).Get("storage").ToObject(h.rt.vm)
	get, _ := goja.AssertFunction(raw.Get("get"))
	stored, err := get(goja.Undefined(), h.rt.vm.ToValue("settings"))
	if err != nil || json.Unmarshal([]byte(stored.String()), &cfg) != nil || cfg.Token != meshToken {
		t.Fatal("real Bolt storage did not persist token")
	}
	meshExpect(t, h.request("POST", "/api/settings", h.admin, map[string]any{"url": "http://127.0.0.1", "token": "abc\r\nx"}, csrf), 400)
	meshExpect(t, h.request("POST", "/api/settings", h.admin, map[string]any{"url": "http://127.0.0.1", "clear_token": true}, csrf), 200)
	if !strings.Contains(h.request("GET", "/api/settings", h.admin, nil, "").Body.String(), `"token_configured":false`) {
		t.Fatal("token was not cleared")
	}
}

func TestMediaMeshHTTPAndNativeTransferContract(t *testing.T) {
	h := meshNew(t)
	var mu sync.Mutex
	var paths []string
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer "+meshToken {
			t.Error("missing server-side bearer credential")
		}
		if r.Header.Get("User-Agent") != "Silo/1.3 (TorrServer)" {
			t.Error("not the real HTTP host")
		}
		mu.Lock()
		paths = append(paths, r.Method+" "+r.URL.RequestURI())
		mu.Unlock()
		w.Header().Set("Content-Type", "application/json")
		switch r.URL.Path {
		case "/api/catalog/search":
			if r.URL.Query().Get("q") != "a&b" || r.URL.Query().Get("tag") != "x/y" || r.URL.Query().Get("limit") != "100" {
				t.Error("query parameters escaped incorrectly")
			}
			fmt.Fprintf(w, `{"results":[{"hash":%q,"title":"<img src=x onerror=alert(1)>","token":%q,"tags":["film"]}]}`, meshHash, meshToken)
		case "/api/catalog/" + meshHash:
			fmt.Fprintf(w, `{"hash":%q,"title":"A & B","torrs":"torrs://not-fed-to-Silo","tags":["film"],"trackers":["udp://127.0.0.1:1234/announce","javascript:alert(1)"],"poster":"http://untrusted.invalid/image"}`, meshHash)
		case "/api/lists", "/api/lists/discover":
			fmt.Fprintf(w, `{"lists":[{"pubkey":%q,"name":"Example list","seq":3,"records":7}]}`, meshKey)
		case "/api/lists/" + meshKey:
			if r.Method != "POST" || r.Header.Get("Content-Type") != "application/json" {
				t.Error("wrong subscribe request")
			}
			w.WriteHeader(202)
			fmt.Fprint(w, `{"status":"subscribing"}`)
		default:
			w.WriteHeader(404)
			fmt.Fprint(w, `{"error":"secret upstream exception"}`)
		}
	}))
	defer server.Close()
	h.configure(server.URL)
	meshExpect(t, h.request("GET", "/api/search?q=a%26b&tag=x%2Fy&limit=9999", h.regular, nil, ""), 200)
	for _, path := range []string{"/api/lists", "/api/discover", "/api/record?hash=" + meshHash} {
		meshExpect(t, h.request("GET", path, h.regular, nil, ""), 200)
	}
	meshExpect(t, h.request("POST", "/api/subscribe", h.admin, map[string]any{"pubkey": meshKey}, h.csrf(h.admin)), 202)
	meshExpect(t, h.request("POST", "/api/subscribe", h.admin, map[string]any{"pubkey": "../other"}, h.csrf(h.admin)), 400)
	meshExpect(t, h.request("GET", "/api/record?hash=../../admin", h.regular, nil, ""), 400)
	// Assert the exact native ParseTorrentSpec limit rather than invent torrs support.
	if _, err := torrent.ParseTorrentSpec("torrs://anything"); err == nil {
		t.Fatal("upstream contract changed: torrs now accepted")
	}
	var calls int
	native := h.rt.vm.Get("ts").ToObject(h.rt.vm).Get("torrent").ToObject(h.rt.vm)
	native.Set("add", func(call goja.FunctionCall) goja.Value {
		calls++
		spec, err := torrent.ParseTorrentSpec(call.Argument(0).String())
		if err != nil || spec.InfoHash.HexString() != meshHash {
			t.Fatalf("native magnet contract: %v", err)
		}
		if len(spec.Trackers) != 1 || spec.Trackers[0][0] != "udp://127.0.0.1:1234/announce" {
			t.Fatalf("trackers: %v", spec.Trackers)
		}
		opts := call.Argument(1).Export().(map[string]any)
		u, err := h.users.Authenticate(h.regular)
		if err != nil || opts["user_id"] != u.ID || opts["save_to_db"] != true || opts["title"] != "A & B" {
			t.Fatalf("native options: %v", opts)
		}
		if _, ok := opts["poster"]; ok {
			t.Fatal("untrusted external poster forwarded")
		}
		return h.rt.vm.ToValue(map[string]any{"hash": meshHash})
	})
	meshExpect(t, h.request("POST", "/api/add", h.regular, map[string]any{"hash": meshHash, "user_id": "owner", "url": "http://attacker.invalid"}, h.csrf(h.regular)), 201)
	if calls != 1 {
		t.Fatal("native add not called exactly once")
	}
	meshExpect(t, h.request("POST", "/api/add", h.regular, map[string]any{"hash": strings.Repeat("a", 64)}, h.csrf(h.regular)), 400)
	mu.Lock()
	defer mu.Unlock()
	if len(paths) != 6 {
		t.Fatalf("unexpected daemon calls: %v", paths)
	}
}

func TestMediaMeshSanitizedDaemonFailures(t *testing.T) {
	h := meshNew(t)
	for _, body := range []string{`{"error":"` + meshToken + `"}`, `not-json`} {
		server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if strings.HasPrefix(body, "{") {
				w.WriteHeader(500)
			}
			fmt.Fprint(w, body)
		}))
		h.configure(server.URL)
		r := h.request("GET", "/api/lists", h.regular, nil, "")
		meshExpect(t, r, 502)
		server.Close()
	}
	meshExpect(t, h.request("GET", "/api/lists", h.regular, nil, ""), 502)
}

func TestMediaMeshRealZIPLoader(t *testing.T) {
	h := meshNew(t)
	archive := filepath.Join(t.TempDir(), "mediamesh.zip")
	f, err := os.Create(archive)
	if err != nil {
		t.Fatal(err)
	}
	writer := zip.NewWriter(f)
	root := os.Getenv("MEDIAMESH_PLUGIN_DIR")
	for _, name := range []string{"info.yaml", "service.js", "index.html", "app.js", "style.css", "lang/en.json", "lang/ru.json"} {
		data, err := os.ReadFile(filepath.Join(root, name))
		if err != nil {
			t.Fatal(err)
		}
		entry, err := writer.Create(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err = entry.Write(data); err != nil {
			t.Fatal(err)
		}
	}
	if err = writer.Close(); err != nil {
		t.Fatal(err)
	}
	if err = f.Close(); err != nil {
		t.Fatal(err)
	}
	reg := &meshRegistrar{}
	reg.Reset()
	manager, err := NewManager(t.TempDir(), h.db, reg, nil, h.users, nil, nil, nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(manager.UnloadAll)
	ids, err := manager.InstallPlugin(archive)
	if err != nil {
		t.Fatal(err)
	}
	if len(ids) != 1 || ids[0] != "mediamesh" {
		t.Fatalf("installed IDs %v", ids)
	}
	manager.tasks.Wait() // InstallPlugin already calls ReloadQueue; startPlugin is asynchronous.
	if manager.runtimes["mediamesh"] == nil || reg.handlers["GET /api/search"] == nil {
		t.Fatal("actual manager did not load plugin")
	}
}
