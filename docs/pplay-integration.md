# pPlay integration requirements

This document defines mandatory implementation boundaries for the P2Play support
module in pPlay. The target client repository is
[`freetushkan/pPlay`](https://github.com/freetushkan/pPlay); its P2Play-specific
implementation belongs in a separate `src/p2play/` source directory. It is separate
from the Silo plugin contract in [silo-plugin.md](silo-plugin.md): pPlay and Silo
are different hosts and must not share assumptions about source layout, runtime
bindings, packaging or identity.

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

## 3. Acceptance checks

The implementation is complete only when tests prove all of the following:

1. The P2Play module can be built, packaged, updated and removed without reading
   from or writing to the TorrServer source tree.
2. A clean TorrServer checkout is not modified by a pPlay module build/test.
3. Legacy TorrServer ping responses remain byte/contract compatible with their
   existing tests.
4. `GET /api/p2play/version` is independently reachable and returns the native
   P2Play version without compatibility prefixes.
5. The returned native version matches the version embedded in the P2Play build;
   changing the application version source updates this endpoint without
   changing legacy ping responses.

The native P2Play version endpoint is implemented in the P2Play server. The
pPlay-specific module and its file-isolated client integration remain future work.
