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

Updated 2026-11-26 against pPlay/dev `402ee80` and P2Play/master `052d569`.
See [pplay-audit.md](pplay-audit.md) for the feature-by-feature TorrServer comparison,
confirmed gaps, security/UI risks and proposed latest-SHA/compiler-cache design.
Implemented below means present in code, not full device acceptance or feature parity.
The dedicated P2Play adapter is independent; some shared UI/audio-artwork paths
still call TorrServe directly and must be migrated to neutral Io operations.

## 1. File-level independence from TorrServer

The P2Play support module inside pPlay MUST be independent from TorrServer at the
file and source-tree level.

- Every file belonging to the P2Play module lives in a P2Play-owned directory and
  has its own path, ownership and build/package entry.
- This rule applies even when a file is a byte-for-byte copy of TorrServer code.
  A copied implementation MUST be stored as a separate P2Play-owned file; it MUST
  NOT reuse, overwrite, patch in place, or be referenced through the original
  TorrServer file path.
- Do not use symlinks, hard links, shared source files, source overlays, generated
  includes pointing into a TorrServer checkout, or build rules that compile a
  TorrServer source file as part of the P2Play module.
- Updating, deleting, packaging or replacing the P2Play module MUST NOT modify or
  require edits to any TorrServer file. The inverse must also hold.
- Keep module-specific tests, resources, manifests and packaging metadata within
  the P2Play-owned tree. Do not install P2Play files into TorrServer directories.
- If upstream code is copied, preserve its applicable copyright/license notices
  and record provenance. File independence does not remove attribution or
  licensing obligations.

Compatibility at the API/protocol boundary is allowed and required where the
pPlay host expects TorrServer-compatible behavior. That compatibility MUST be
implemented by the independent P2Play module files; it is not a reason to share
TorrServer files.

The neutral pPlay host dispatcher (`Io`) may select a backend and call its adapter.
It is the integration boundary, not part of the P2Play adapter. Shared playback and
media-probing code must use neutral `Io` operations; it must not include or invoke
`TorrServe` or `P2Play` APIs directly. The P2Play-owned adapter files (`p2play.h/.cpp`)
may use common HTTP/configuration primitives, but MUST NOT include, call or depend on
TorrServer adapter files, paths, saved-copy storage or TorrServer-specific helpers.

## 2. Separate P2Play product identity and version endpoint

TorrServer compatibility probes and P2Play product identity are different
contracts. Existing TorrServer-compatible ping endpoints remain available and
must keep their existing response formats, including their compatibility
identifiers/prefixes.

P2Play exposes its own read-only, public version endpoint:

`GET /api/p2play/version`

The route is handled before the native API token check, so clients can identify
the product before configuring credentials. Its implementation is separate from
the legacy ping and Silo/MatriX version routes.

Canonical response:

```json
{
  "name": "P2Play",
  "version": "0.1"
}
```

The `version` value is the native P2Play application version from the project's
single version source/build metadata. It MUST NOT be the MatriX, TorrServer,
Silo, or other compatibility label and MUST NOT contain a compatibility prefix.
The endpoint must not be an alias for a legacy ping endpoint.

The P2Play endpoint and the compatibility probes serve different purposes:

| Interface | Purpose | Identity |
| --- | --- | --- |
| TorrServer MatriX ping/echo | Client compatibility detection | Existing MatriX/TorrServer-compatible response |
| TorrServer Silo ping | Silo compatibility detection | Existing Silo-compatible response |
| `GET /api/p2play/version` | Identify the actual P2Play product/build | `P2Play` and its native version only |

Do not change legacy ping responses to make them look like the native P2Play
version endpoint. Do not make pPlay infer the native P2Play version by parsing a
TorrServer compatibility string.

## 3. External pPlay client baseline

The external `P2Play` network integration is implemented in `freetushkan/pPlay/dev`.
Its adapter is isolated in `src/net/p2play.h/.cpp`; `Io` is the only backend dispatcher.
Playback progress and FFmpeg `MediaProbe` use neutral operations. This boundary
is not yet universal: `player_visual.cpp::pictureUrlsOf` still calls TorrServe
directly; see the audit before claiming complete shared-player independence.

Completed external-client features:
- native `GET /api/p2play/version` identity check, Bearer login and account-scoped API calls;
- MediaMesh discovery/subscriptions, search, selected-list filtering, record details,
  lazy poster/description loading, add-to-library and authenticated playback;
- per-network user management (PR #8): list/create/change password/change rank/delete,
  with owner protection and rank-filtered role choices;
- per-user playback progress read/write and position restore (PR #9), preserving the
  existing TorrServer path behind the neutral dispatcher;
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
  share the data directory of an external P2Play daemon or any TorrServer data.
- The C API is the lifecycle boundary: pPlay starts, observes and stops the core
  in-process. The standalone daemon remains independently buildable and usable.

### LAN access and authentication

The embedded HTTP server keeps its existing wildcard listener
(`INADDR_ANY` for IPv4). Do not add loopback-only, interface, subnet, allowlist or
other LAN filtering. Reachability is determined by ordinary routing, NAT and the
device's firewall configuration; P2Play/pPlay must not introduce an extra LAN
network policy or attempt to configure NAT/port forwarding.

The embedded server keeps the existing fixed owner identity: username `owner` and
internal ID `owner`. pPlay passes the password from the `P2Play_embedded` network
settings through `p2play_config.owner_password` to `p2play_start()`. On every
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

## 5. Acceptance checks

The implementation is complete only when tests prove all of the following:

1. The P2Play module can be built, packaged, updated and removed without reading
   from or writing to the TorrServer source tree.
2. P2Play-owned files (including byte-for-byte copied implementations) have their
   own paths and compile/package entries. No `torrserve.*` implementation file,
   symlink, hard link or generated source overlay is reused.
3. A clean TorrServer checkout is not modified by a pPlay module build/test.
4. Legacy TorrServer ping responses remain byte/contract compatible with their
   existing tests.
5. `GET /api/p2play/version` is independently reachable and returns the native
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
13. The current numeric `rank` hierarchy is documented as such. A separate
    follow-up defines named roles and an explicit capability/permission matrix.

The native identity, external adapter, Continue Watching, embedded lifecycle,
owner provisioning and embedded reuse of user management are implemented in code.
Physical LAN/lifecycle acceptance remains open. Full TorrServer parity is not
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
