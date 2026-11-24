# Publisher / releaser

The DOCX defines the releaser as P2Play core running with a private Ed25519 list
key. The key is generated/stored by P2Play; Grabber never receives it. Releaser
decides public fields, uploads media, signs releases and publishes versions.

## Existing API

- POST /api/publish — create list and generate key.
- GET /api/publish/:pubkey/status — pending/release state.
- POST /api/publish/:pubkey/records — NDJSON import.
- POST /api/publish/:pubkey/media/:kind — upload media and obtain signed ref.
- POST /api/publish/:pubkey/release — release next version.
- POST /api/publish/:pubkey/snapshot — compact history/media.
- GET/POST /api/lists/:pubkey/policy — shard selection and budgets.

These APIs exist. The separate CLI `p2play publish --import`, GUI wrapper,
scheduler and policy editor do not. Implement CLI first, GUI later. Qt belongs
only in the wrapper, never in the shared Qt-free core.

## Import compatibility

The DOCX proposes interchange fields `ih,name,size,files,ih2,date,tags,desc,
poster,file_list,trackers,site,src`. This is not MediaMesh v1. Current parser
accepts `ih,title,shard,date,size,files,tags,trackers,poster,description_ref,
poster_ref`. Convert name→title and dates to integer timestamps.

- `ih2`, `site`, `src`, `file_list` need explicit schema extensions.
- Upload description/poster bytes through media endpoints and store returned
  signed refs; do not put SHA-256 pseudo-URLs in legacy poster.
- Whitelist fields, map categories to tags and resolve source conflicts by priority.

## Workflow

Create/select normal/meta lists; configure whitelist, priorities, tag mapping,
shards and budgets. Import batches, report accepted/duplicate/rejected/filtered,
upload permitted media, release and show sequence/segment/pending/network status.
DOCX starting compaction heuristics: >64 segments or >20% deletions; configurable,
not protocol hard limits. Also support scheduled/threshold release.

## Network and security

Support SOCKS5 UDP ASSOCIATE or network namespace via WireGuard/AmneziaWG.
Grabber uses same tunnel and respects rate limits. Redact secrets. Private keys
never leave P2Play.

## Acceptance

Test idempotent imports, media refs, field whitelist, scheduled/threshold release,
snapshot policy and second-node subscription. Verify tunnel mode has no direct
network fallback.
