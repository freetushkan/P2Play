# pPlay integration requirements

This document defines mandatory implementation boundaries for the P2Play support
module in pPlay. The target client repository is
[`freetushkan/pPlay`](https://github.com/freetushkan/pPlay); its P2Play-specific
implementation may live in separate files such as `src/net/p2play.h` and
`src/net/p2play.cpp` alongside pPlay's neutral networking helpers. A dedicated
directory is optional, not mandatory. It is separate from the Silo plugin contract
in [silo-plugin.md](silo-plugin.md): pPlay and Silo are different hosts and must not
share assumptions about source layout, runtime bindings, packaging or identity.

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

## 3. Embedded pPlay mode

pPlay has two distinct network entries:

- `P2Play`: the existing external-server client.
- `P2Play_embedded`: the same P2Play service hosted inside the pPlay process.

The embedded entry MUST use the static `p2play_core` library, not launch or bundle
the standalone `p2playd` application. pPlay's build workflow clones the P2Play
repository into its build workspace and checks out a pinned commit; source files
are not manually copied from one repository into the other. A dedicated
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

The embedded HTTP server is a LAN service, not a loopback-only service. It MUST
listen on an address reachable by other devices on the same local network; binding
only to `127.0.0.1` is not acceptable. pPlay should display or expose the LAN
address and port. It must not silently configure router port forwarding or claim
that LAN visibility implies safe Internet exposure.

The username and password entered in the `P2Play_embedded` network settings are
the actual credentials of the embedded server's owner account. The username is
user-defined and MUST NOT be hard-coded to `owner`. The backend may retain a
stable internal owner ID (for example, `id=owner`), but Basic login, native API
login, tokens and account display must use the configured username.

On first initialization, the configured username/password provision the persistent
owner account and the password is stored only as a password hash. On subsequent
starts, the stored account is authoritative: the embedded core must verify that
the configured credentials still identify that owner and MUST NOT silently reset
or overwrite an existing account database. A credential mismatch must be surfaced
as a startup/configuration error, not bypassed by creating a new owner.

### Additional users and roles

The embedded server must expose creation of additional server users from pPlay's
user-facing settings/management UI, using the existing authenticated user-management
API where possible. The created users and their permissions must persist across
embedded-server restarts. The owner remains protected from deletion or demotion.

The current backend already has user management and a numeric hierarchical
`rank` field. That rank is a coarse privilege level, not a named-role system or a
per-action capability matrix. A later access-control milestone must define explicit
roles and permissions (including which actions each role may perform) before pPlay
presents role assignment as a user-friendly feature. Do not describe the current
numeric rank as if named roles were already implemented.

## 4. Acceptance checks

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
7. The embedded build links the static core through the pinned P2Play checkout and
   can omit the standalone daemon target without changing the standalone build.
8. Selecting `P2Play_embedded` starts the service lazily; leaving it drains active
   streams, stops the service and releases its listening port. Selecting any other
   network does not start the embedded service.
9. A second device on the same LAN can reach the embedded HTTP service. The service
   is not restricted to loopback and does not set up public WAN port forwarding.
10. The username/password from pPlay's embedded-network settings authenticate the
    actual owner account, including when its username is not `owner`. The first
    initialization persists the owner; later starts do not silently overwrite
    existing credentials or user data.
11. The owner can create additional server users through the pPlay management UI;
    their accounts persist across restarts and the owner cannot be deleted.
12. The current numeric `rank` hierarchy is documented as such. A separate
    follow-up defines named roles and an explicit capability/permission matrix.

The native P2Play version endpoint is implemented in the P2Play server. The external
pPlay client baseline exists; the embedded lifecycle, LAN deployment, owner
provisioning and pPlay-facing user-management UI remain implementation work.
