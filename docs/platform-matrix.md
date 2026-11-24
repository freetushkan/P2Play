# Platform and runtime matrix

Updated: 2026-11-24. This matrix distinguishes code/build evidence from device-level
acceptance. "Built" does not imply that the integrated lifecycle has been tested.

## Intended product layout

| Platform | P2Play runtime | pPlay integration | Silo MediaMesh plugin |
| --- | --- | --- | --- |
| Linux | Standalone `p2playd` | External `P2Play` client exists; `P2Play_embedded` is planned | Supported with an external P2Play daemon; Linux live acceptance test passes |
| Android | Standalone P2Play server is packaged in the Android APK and launched as a foreground service | External client builds; `P2Play_embedded` is planned | Android Silo server binaries and plugin runtime source exist; installation/use with an Android P2Play service is plausible but not yet device-verified |
| Switch | No standalone daemon is part of the target product layout | `P2Play_embedded` is the target | Not a target |
| PS4 | No standalone daemon is part of the target product layout | `P2Play_embedded` is the target | Not a target |
| PS5 | No standalone daemon is part of the target product layout | `P2Play_embedded` is the target | Not a target |

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
- pPlay PR #6 established the PR workflow for Linux, Android, Switch, PS4 and PS5.
  Subsequent PR #8 (per-network user management) and PR #9 (per-user progress sync,
  duration fallback and backend isolation) also passed that five-platform workflow.
  External `P2Play` is implemented; the targets do not yet link `p2play_core` and there is
  no `P2Play_embedded` module in pPlay today.
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
2. **pPlay embedded (planned):** link the static `p2play_core` into pPlay's
   Android native application and control it through the pPlay network module.
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
  Silo/plugin stack.
