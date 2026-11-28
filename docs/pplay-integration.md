# pPlay integration requirements

This document defines mandatory implementation boundaries for the P2Play support
module in pPlay. The target client repository is
[`freetushkan/pPlay`](https://github.com/freetushkan/pPlay); its P2Play-specific
implementation may live in separate files such as `src/net/p2play.h` and
`src/net/p2play.cpp` alongside pPlay's neutral networking helpers. A dedicated
directory is optional, not mandatory. It is separate from the Silo plugin contract
in [silo-plugin.md](silo-plugin.md): pPlay and Silo are different hosts and must not
share assumptions about source layout, runtime bindings, packaging or identity.

## Current audit and implementation status

Updated 2026-11-28 against pPlay/dev
`23444a68e920329f09c7dbef89ad39c00a18afb2` and P2Play/master
`f5a70868f791a2e1dfc07654767adde0383d7098`.
See [pplay-audit.md](pplay-audit.md) for the feature-by-feature legacy backend comparison,
confirmed gaps, security/UI risks and proposed latest-SHA/compiler-cache design.
Implemented below means present in code, not full device acceptance or feature parity.
The dedicated P2Play adapter is independent; some shared UI/audio-artwork paths
still call legacy adapter directly and must be migrated to neutral Io operations.

A working pPlay implementation branch, `feature/p2play-library-management`
(commit `1fdebdebe5bde6408923f1f55cd7177aff193a44`, based directly on `dev`),
adds native library query/category/sort, add magnet/link and multipart `.torrent`,
title/category editing and per-user deletion UI. It is intentionally not a PR.
It has not been built or run through Actions/device acceptance; do not treat its
branch-only code as validated or merged `dev` behavior.

## 1. File-level independence from legacy backend

The P2Play support module inside pPlay MUST be independent from legacy backend at the
file and source-tree level.

- Every file belonging to the P2Play module lives in a P2Play-owned directory and
  has its own path, ownership and build/package entry.
- This rule applies even when a file is a byte-for-byte copy of legacy backend code.
  A copied implementation MUST be stored as a separate P2Play-owned file; it MUST
  NOT reuse, overwrite, patch in place, or be referenced through the original
  legacy backend file path.
- Do not use symlinks, hard links, shared source files, source overlays, generated
  includes pointing into a legacy backend checkout, or build rules that compile a
  legacy backend source file as part of the P2Play module.
- Updating, deleting, packaging or replacing the P2Play module MUST NOT modify or
  require edits to any legacy backend file. The inverse must also hold.
- Keep module-specific tests, resources, manifests and packaging metadata within
  the P2Play-owned tree. Do not install P2Play files into legacy backend directories.
- If upstream code is copied, preserve its applicable copyright/license notices
  and record provenance. File independence does not remove attribution or
  licensing obligations.

P2Play is now a standalone product with its own versioned REST API. The pPlay
P2Play adapter talks only to `/api/v1`; it must not require MatriX or Silo
compatibility endpoints. The separate legacy backend adapter remains an independent
backend and is not part of P2Play core.

The neutral pPlay host dispatcher (`Io`) may select a backend and call its adapter.
It is the integration boundary, not part of the P2Play adapter. Shared playback and
media-probing code must use neutral `Io` operations; it must not include or invoke
`legacy adapter` or `P2Play` APIs directly. The P2Play-owned adapter files (`p2play.h/.cpp`)
may use common HTTP/configuration primitives, but MUST NOT include, call or depend on
legacy adapter files, paths, saved-copy storage or backend-specific helpers.

## 2. Separate P2Play product identity and version endpoint

P2Play product identity is a native contract. P2Play exposes a native product identity endpoint. The old compatibility probes
are not part of P2Play and must not be reintroduced into its core.

P2Play exposes its own read-only, public version endpoint:

`GET /api/v1/system/version`

The route is handled before the native API token check, so clients can identify
the product before configuring credentials. Its implementation is separate from
the native `/api/v1/system/ping` and `/api/v1/system/version` routes.

Canonical response:

```json
{
  "name": "P2Play",
  "version": "0.1"
}
```

The `version` value is the native P2Play application version from the project's
single version source/build metadata. It MUST NOT be the MatriX, legacy backend,
Silo, or other compatibility label and MUST NOT contain a compatibility prefix.
The endpoint must not be an alias for a legacy ping endpoint.

The P2Play endpoint and the compatibility probes serve different purposes:

| Interface | Purpose | Identity |
| --- | --- | --- |
| `GET /api/v1/system/version` | Identify the P2Play product/build | `P2Play` and its native version only |

Do not change legacy ping responses to make them look like the native P2Play
version endpoint. Do not make pPlay infer the native P2Play version by parsing a
legacy backend compatibility string.

## 3. External pPlay client baseline

The external `P2Play` network integration is implemented in `freetushkan/pPlay/dev`.
Its adapter is isolated in `src/net/p2play.h/.cpp`; `Io` is the only backend dispatcher.
Playback progress and FFmpeg `MediaProbe` use neutral operations. This boundary
is not yet universal: `player_visual.cpp::pictureUrlsOf` still calls legacy adapter
directly; see the audit before claiming complete shared-player independence.

Completed external-client features:
- native `GET /api/v1/system/version` identity check, Bearer login and account-scoped API calls;
- MediaMesh discovery/subscriptions, search, selected-list filtering, record details,
  lazy poster/description loading, add-to-library and authenticated playback;
- per-network user management (PR #8): list/create/change password/change rank/delete,
  with owner protection and rank-filtered role choices;
- per-user playback progress read/write and position restore (PR #9), preserving the
  existing legacy backend path behind the neutral dispatcher;
- duration retention and FFmpeg `MediaProbe` fallback when mpv has no duration (PR #9).

PR #8 and PR #9 were merged into `dev`; their CI workflows passed for Linux, Android,
Switch, PS4 and PS5. This is cross-platform build evidence, not a claim of manual
acceptance on every physical device. Continue Watching UI is now implemented
through the per-user progress endpoint and refreshes after playback.

The external client and the separately implemented embedded lifecycle share the
HTTP adapter; embedded startup/drain/stop is implemented in `p2play_embedded.cpp`.

## 4. Embedded pPlay mode

pPlay has two distinct network entries:

- `P2Play`: the existing external-server client.
- `P2Play_embedded`: the same P2Play service hosted inside the pPlay process.

The embedded entry MUST use the static `p2play_core` library, not launch or bundle
the standalone `p2playd` application. pPlay's build workflow clones the P2Play
repository into its build workspace and checks out a pinned commit; source files
are not manually copied from one repository into the other. Current workflow pin is `1f30ceebd253cd4a71e3b978fcf0e8b70ac1f3dd`, while
`ci/prepare-p2play-embedded.sh` has fallback `e33886c383cd0e9d9645777ec1ac73a18711a88f`.
This mismatch must be removed. Proposed dev policy resolves latest master once
to a shared immutable SHA per workflow; releases retain exact rebuild metadata.
This proposal is not implemented by this documentation patch. A dedicated
`P2P_EMBEDDED_ONLY` CMake configuration should build the embeddable core/API
without the standalone application target.

### Activation and storage

- The embedded core starts only after the user selects `P2Play_embedded`. Merely
  launching pPlay or selecting the external `P2Play` network MUST NOT start it.
- When the user leaves the embedded network for another module, pPlay stops the
  embedded server after in-flight HTTP handlers and active media streams have
  drained. It must not terminate a stream halfway through or leave a listener
  running after deactivation.
- The embedded server uses a dedicated data directory owned by pPlay. It must not
  share the data directory of an external P2Play daemon or any legacy backend data.
- The C API is the lifecycle boundary: pPlay starts, observes and stops the core
  in-process. The standalone daemon remains independently buildable and usable.

### LAN access and authentication

The embedded HTTP server defaults to port `8660`. pPlay exposes a per-network
port setting for the Built-in instance (valid TCP ports 1–65535; zero and values
outside the range are rejected). Changing the port restarts the embedded service
and updates its base URL and poster transport context.

The listener defaults to `127.0.0.1` for a newly created Built-in P2Play network.
The user may disable this restriction to expose the service on the device's
network interfaces (`INADDR_ANY` for IPv4). When loopback-only mode is enabled,
network sharing is unavailable. The UI offers network sharing only on Android.

The embedded server keeps the existing fixed owner identity: username `owner` and
internal ID `owner`. pPlay passes the password from the P2Play network settings through
`p2play_config.owner_password` to `p2play_start()`. A new Built-in P2Play
network receives a randomly generated owner password. On every
embedded start, that password is applied to the owner's password hash, replacing
the previous password when it changes. The owner ID, username, API token and all
other user records MUST remain unchanged. A NULL `owner_password` preserves the
standalone server's existing first-login password flow.

### Embedded logging

The embedded core must write to the host application's existing log, not to a
separate P2Play log file. pPlay supplies `p2play_config.log` and
`p2play_config.log_user` when starting the core. The callback forwards each core
message to pPlay's normal logger at `Utility::LogLevel::Info`, prefixed with
`P2Play:` (for example, `Utility::log(Utility::LogLevel::Info, "P2Play: " + line)`).

pPlay's configured log threshold controls visibility: core messages appear in
`pplay.log` when the selected level is Info, Debug or Trace, and are filtered
when the selected level is Off or Error. Do not bypass the pPlay log filter or
write directly to another file. The core's existing credential redaction in its
C API log path must remain in effect.

### Additional users and roles

The embedded server must expose creation of additional server users from pPlay's
user-facing settings/management UI, using the existing authenticated user-management
API where possible. The created users and their permissions must persist across
embedded-server restarts. The owner remains protected from deletion or demotion.

The current backend already has user management and a numeric hierarchical
`rank` field. That rank is a coarse privilege level, not a named-role system or a
per-action capability matrix. The current UI already offers Administrator/User/Guest presets over numeric rank;
see [users-and-roles.md](users-and-roles.md). A later milestone must define a full
per-action capability matrix; do not confuse rank presets with such a matrix.

## 4a. Data location of the embedded module

Everything the embedded core keeps belongs to pPlay and must live inside pPlay's own data
directory. The host passes that directory as `p2play_config.data_dir`; pPlay uses
`<pPlay data>/p2play_embedded/`, so the library, settings, metadata, disk storage, the
MediaMesh lists and the cover cache are all nested in pPlay's tree:

```
<pPlay data>/p2play_embedded/library, settings.json, metadata/, torrent-cache/, lists/,
             covers/<infohash>.img, covers/<infohash>.json
```

- The embedded core MUST NOT write outside that directory, must not use an absolute or
  system-wide path of its own, and must not share the directory of an external P2Play
  daemon or of any legacy backend data.
- Removing or resetting the pPlay data directory removes the embedded server's data with
  it; a backup of pPlay's data directory is a complete backup of the embedded server.
- The same rule applies to caches: they are caches of that instance, not of the device.
  A second pPlay instance (another user, another OS profile) gets its own directory and
  its own caches; nothing is shared through `/tmp`, a global cache directory or XDG paths.

## 4b. Cover images and the double-cache rule

The server extracts a cover for a torrent added without a poster (an image file inside the
torrent, then a frame of its video through ffmpeg) and caches it by infohash. Contract in
[`covers.md`](covers.md); what pPlay must do:

- A poster the client sends (`poster` in `POST`/`PUT /api/v1/torrents`) wins. The server
  never replaces it and reports `cover: false`. pPlay must keep passing its own poster when
  it has one (catalog details, user choice) instead of relying on extraction.
- `GET /api/v1/torrents/{hash}/cover` answers `200` with the bytes, `ETag` and
  `Cache-Control: private, max-age=86400`, or `304` for a client that already has them.
  The listing carries only `cover`/`cover_source`, never the bytes.
- **Embedded network:** the cache already lives in
  `<pPlay data>/p2play_embedded/covers/<infohash>.img`, inside pPlay's directory. pPlay
  MUST NOT copy those bytes into `data/cache/posters`; it reads the file (or asks the
  server for a path) and uses it directly. One file, one owner.
- **External server:** pPlay keeps its own `PosterCache`, keyed `p2play-cover:<host>:<infohash>`,
  which already stores one file per picture by md5. It must revalidate with the stored
  `ETag` instead of re-downloading, and it must not keep a second copy of a picture it also
  has locally.
- Do not attach the cover cache to the download directory, the saved/offline storage or the
  MediaMesh media packs: covers are per-torrent metadata and disappear with the last
  library entry.

## 5. Acceptance checks

The implementation is complete only when tests prove all of the following:

1. The P2Play module can be built, packaged, updated and removed without reading
   from or writing to the legacy backend source tree.
2. P2Play-owned files (including byte-for-byte copied implementations) have their
   own paths and compile/package entries. No `previous backend files` implementation file,
   symlink, hard link or generated source overlay is reused.
3. A clean legacy backend checkout is not modified by a pPlay module build/test.
4. Legacy backend ping responses remain byte/contract compatible with their
   existing tests.
5. `GET /api/v1/system/version` is independently reachable and returns the native
   P2Play version without compatibility prefixes.
6. The returned native version matches the version embedded in the P2Play build;
   changing the application version source updates this endpoint without
   changing legacy ping responses.
7. The embedded build links the static core through one immutable resolved P2Play SHA and
   can omit the standalone daemon target without changing the standalone build.
8. Selecting `P2Play_embedded` starts the service lazily; leaving it drains active
   streams, stops the service and releases its listening port. Selecting any other
   network does not start the embedded service.
9. The embedded HTTP listener retains wildcard bind (`INADDR_ANY` for IPv4).
   No loopback-only, interface, subnet or other LAN filtering is added; reachability
   remains a function of ordinary network routing, NAT and firewall.
10. Core log callback messages appear in the existing `pplay.log` via pPlay's
    `Utility::log(LogLevel::Info, ...)`. They are visible at Info/Debug/Trace and
    filtered at Off/Error; no separate P2Play log file is created.
11. The configured embedded password is applied to the fixed `owner` account on
    every server start. Changing it changes password authentication but preserves the
    owner's stable ID, username, API token and all additional user records.
12. The owner can create additional server users through the pPlay management UI;
    their accounts persist across restarts and the owner cannot be deleted.
13. A torrent added without a poster gets a cover from its own files within the metadata
    limits, or an ffmpeg frame where that binary exists; a poster sent by the client is
    never replaced. The cache is `<data_dir>/covers/<infohash>.img`, i.e. inside pPlay's
    data directory for the embedded module.
14. The current numeric `rank` hierarchy is documented as such. A separate
    follow-up defines named roles and an explicit capability/permission matrix.

Covers are implemented on the server side (`GET/POST/DELETE /api/v1/torrents/{hash}/cover`,
`cover`/`cover_source` in the library cards, the `CoverProbe` setting) and are covered by
`tools/ui-test.py`; the pPlay side of 4b is a requirement, not implemented there yet.

The native identity, external adapter, Continue Watching, embedded lifecycle,
owner provisioning and embedded reuse of user management are implemented in code.
Physical LAN/lifecycle acceptance remains open. Full legacy backend parity is not
complete: watched/reset/folder UI, library listing/deletion, offline storage and
shared artwork need the work recorded in [pplay-audit.md](pplay-audit.md).

## 6. Additional requirements from the parity audit

- Route playback, artwork, server progress and watched-state operations through
  neutral host interfaces, preserving file-level adapter independence.
- Manual watched/reset actions must update the authenticated server state, not
  merely local watch-later. Define reset of position and viewed together.
- Do not block player/UI updates on synchronous remote progress writes; use a
  bounded ordered queue with final-write/reset/account-change guarantees.
- Redact stream-token URLs before logging; never store credentials in build caches.
- Support library browse/delete or clearly expose their absence as capabilities.
- Keep one resolved core SHA across all platform jobs/Android ABIs, record build
  provenance, and invalidate dependency/compiler caches using actual toolchain,
  flags, headers and patch fingerprints. See the audit for acceptance criteria.


## 5. Stable tokens and HTTPS poster transport

P2Play owns one durable API token per user. Login returns the existing token; it
does not rotate it. The user or an administrator with sufficient rank may explicitly
reset it. pPlay keeps the token in its in-memory account/session cache and sends it
as `Authorization: Bearer` for authenticated API calls. It is not shown in network
settings. The browser's share-link operation is separate: it asks the server to
create a URL containing the user's token for an individual file stream.

For poster URL fetching in the embedded core, reuse pPlay's existing
`Http::perform` transport. The callback must use `Io::tlsVerify(url, context)`
so the selected P2Play network's certificate-verification setting applies to
external poster hosts too. Certificate verification is enabled by default and
must not be disabled automatically on errors. Redirects are returned to the core
one hop at a time; the core revalidates every target before the next request.
Do not instantiate another HTTP/TLS client in the embedded core.
