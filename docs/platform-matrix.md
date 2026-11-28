# Platform and runtime matrix

Updated: 2026-11-28. This matrix distinguishes code/build evidence from device-level
acceptance. "Built" does not imply that the integrated lifecycle has been tested.

## Intended product layout

| Platform | P2Play runtime | pPlay integration | Silo MediaMesh plugin |
| --- | --- | --- | --- |
| Linux | Standalone `p2playd` | External `P2Play` client and `P2Play_embedded` are implemented; embedded core passes CI, device lifecycle acceptance pending | Supported with an external P2Play daemon; Linux live acceptance test passes |
| Android | Standalone P2Play server is packaged in the Android APK and launched as a foreground service | External client and `P2Play_embedded` are implemented; embedded core is included in Android CI, device acceptance pending | Android Silo server binaries and plugin runtime source exist; installation/use with an Android P2Play service is plausible but not yet device-verified |
| Switch | No standalone daemon is part of the target product layout | `P2Play_embedded` is implemented and built in CI; physical-device acceptance pending | Not a target |
| PS4 | No standalone daemon is part of the target product layout | `P2Play_embedded` is implemented and built in CI; physical-device acceptance pending | Not a target |
| PS5 | No standalone daemon is part of the target product layout | `P2Play_embedded` is implemented and built in CI; physical-device acceptance pending | Not a target |

## Native API build policy

P2Play ships one native, versioned REST API under `/api/v1`. The MatriX
compatibility router, HTTP Basic authentication and old unversioned Silo-compatible
REST routes are removed from standard builds and are not optional build features.
The `torrs://` URI and `.torrs` card format remain application data formats used
by Android/pPlay; their retention does not imply legacy API emulation.

## Current evidence

- P2Play Linux standalone daemon: Linux CI, protocol tests, MediaMesh tests and
  Silo live acceptance run against a real daemon.
- P2Play Android standalone server: `ci/android-apk.sh` builds `p2playd` for
  `arm64-v8a` and `armeabi-v7a`, packages it into the APK and starts it from
  Android's foreground `ServerService` on port 8090. This is not the pPlay
  embedded mode.
- Silo plugin on Linux: the plugin is tested against the pinned Silo Goja runtime;
  Linux live acceptance verifies discovery, search, native add and authenticated
  streaming through the external P2Play daemon.
- Silo on Android: the upstream Silo.3 release includes Android server binaries,
  and the pinned Silo source contains the same plugin manager/Goja runtime code.
  However, no Android device/emulator acceptance test currently proves plugin
  installation, route mounting or the local P2Play HTTP call. Treat this as a
  feasible candidate, not a completed deployment.
- Audited pPlay `dev` `402ee80a6c7271622659abefb390ab4999ca9009` includes
  external and embedded clients. Main workflow selects core
  `1f30ceebd253cd4a71e3b978fcf0e8b70ac1f3dd`; prepare script fallback is a different
  SHA (`e33886c...`), a tracked build-policy defect. Static `p2play_core` linkage,
  lazy start, drain, dedicated data directory, owner password and host logging are
  present in code. Latest observed successful public workflow is for `de03419...`,
  run `45752020074`, not audited dev HEAD. No fresh physical-device acceptance was
  performed. Earlier run #1475 remains historical evidence only. Full parity,
  runtime risks and a proposed single-resolved-SHA/cache design are in
  [pplay-audit.md](pplay-audit.md). Do not infer runtime verification of latest
  P2Play master from a build using an older core SHA.
- The Silo MediaMesh plugin is an HTTP client for an external P2Play daemon. The
  current Goja plugin API exposes no process launcher or `p2play_core` C API.
  To let the plugin manage its own local core, add a host-level native lifecycle
  bridge and package the platform binary/library with the host. That is separate
  work from the existing JavaScript plugin.

## Android deployment options

1. **External daemon (available architecture):** run the P2Play Android APK's
   foreground service on the device. The Silo MediaMesh plugin can be configured
   to call the daemon through `http://127.0.0.1:8090` when Silo and P2Play run
   on that same device, or use a reachable remote daemon.
2. **pPlay embedded (implemented):** the Android native application links the
   static `p2play_core` through the pPlay network module; build integration is covered
   by the five-platform CI. Device-level lifecycle/LAN acceptance remains pending.
3. **Silo-managed embedded core (not implemented):** would need a native host
   lifecycle API or launcher. Plugin JavaScript alone cannot spawn a daemon or
   call the P2Play C API with the bindings currently exposed.

Before calling option 1 supported, add an Android device/emulator acceptance
test for installing the plugin, authenticating to the local P2Play service,
background operation, stop/restart and persistent data.

## Scope boundary

- Linux may legitimately ship all three modes: standalone daemon, pPlay embedded,
  and Silo plugin connected to a standalone daemon.
- Android targets pPlay embedded and may also host the Silo plugin connected to a
  separately running P2Play Android service; the latter remains unverified.
- Switch, PS4 and PS5 target the embedded pPlay core, not a separately deployed
  Silo/plugin stack. Embedded code/build integration is complete; physical-device
  LAN and lifecycle acceptance remains separate from CI build evidence.

## CI dependency reliability

P2Play PR #35 merged Android Gradle dependency-cache setup and repository ordering.
The subsequent HTTP 429 from Maven Central exposed that trusted same-repository PR
builds also need to seed the Gradle cache. The follow-up adds Google's Maven Central
mirror (`maven-central.storage-download.googleapis.com/maven2/`) before Maven Central,
keeps Central as fallback, permits cache writes for trusted same-repository PRs/pushes,
keeps fork PR caches read-only, and preserves partial cache state after failed builds.
P2Play run #339 passed all five platform jobs and the PS5 package job; pPlay PR #14
merged corresponding changes and run #1485 passed. This addresses dependency-resolution
reliability, not every possible Android build failure.

## Outbound HTTP(S) fetch transport

- **Linux standalone:** libcurl with peer/hostname verification enabled. No
  application CA file is forced; the system libcurl trust configuration is used.
  The shared path imports remote `.torrent` files (up to 16 MiB) and posters
  (up to 2 MiB).
- **Android standalone:** an authenticated loopback-only bridge from `p2playd`
  to Java `SSLSocket`. The bridge uses Android's system CA store and hostname
  verification; the daemon supplies the DNS-pinned IPv4 address. Its bounded
  response capacity supports both poster and torrent-file limits.
- **Switch standalone:** trusted certificates are exported at startup from the
  current libnx SSL certificate store and loaded by libcurl.
- **PS4 / PS5 standalone:** libcurl uses the CA bundle at
  `data/ca/cacert.pem`, generated from the same bundle used by pPlay builds.
- **pPlay embedded:** PR [pPlay #17](https://github.com/freetushkan/pPlay/pull/17)
  supplies a host callback backed by pPlay's existing `Http::perform`, platform
  CA configuration and `Io::tlsVerify()` preference. The callback carries the
  DNS pin and disables automatic redirects/proxies for the request. There is no
  separate TLS implementation
  in the MediaMesh Silo plugin; it only searches signed catalogs and passes a
  selected magnet to Silo's own torrent host API.

All hosts return redirect locations to the core without automatic following.
The shared fetcher is bounded per request: 2 MiB for posters, 16 MiB for torrent
metadata. It re-resolves and validates every redirect target (public IPv4 only),
pins the selected address into the transport, and rejects HTTPS-to-HTTP downgrade.
See [HTTP(S) fetch policy and tests](http-fetch.md).
The core re-resolves and checks each target and rejects HTTPS-to-HTTP downgrade.
