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

The standard-library CLI now lives in `tools/p2play` and
`tools/p2play_publisher.py`. It implements `p2play publish --import` with
local validation, dry-run, owned-list selection or list creation, Bearer login,
category mapping, output whitelist, description/poster upload and an explicit
`--release` switch. It does not contain or receive the Ed25519 private key.
The GUI wrapper, scheduler and policy editor remain future work. A versioned JSON profile (`--profile`) now supplies field whitelist, category mapping, default/category shard routing and ordered source enablement/priority metadata. CLI flags for whitelist, category map and default shard override profile values. The publisher validates source priorities but does not merge sources; that remains the grabber's responsibility. Example: `tools/publisher-profile.example.json`. Qt belongs
only in the wrapper, never in the shared Qt-free core.

The grabber must merge source layers before calling this CLI. The CLI skips
identical duplicate rows and rejects conflicting records for the same infohash;
it deliberately does not apply source priorities or perform metadata upserts.

Run it from a checkout:

```sh
export P2PLAY_PASSWORD='...'
tools/p2play publish --import catalog.ndjson --server https://p2play.example \
  --list <64-hex-public-key> --dry-run
tools/p2play publish --import catalog.ndjson --server https://p2play.example \
  --list <64-hex-public-key>
tools/p2play publish --import catalog.ndjson --server https://p2play.example \
  --create-list 'My catalog' --description 'Public catalog' --release
```

Without `P2PLAY_PASSWORD`, the CLI prompts without echoing the password.
Credentials over non-loopback plain HTTP are refused unless the operator
explicitly passes `--allow-insecure-http`. The CLI never follows redirects
with an Authorization header and never automatically retries mutating POSTs.
A network failure during a media upload may leave unreferenced staged bytes;
inspect publisher status before retrying. Import and release are separate:
without `--release`, records remain pending for review.

## Import compatibility

The canonical record schema is `ih,title,shard,date,size,files,tags,trackers,
poster,description_ref,poster_ref` (or a deletion record with `del` and
`shard`). The CLI accepts the useful interchange alias `name` for `title`,
and ISO-8601 or Unix-second dates. It rejects conflicting `name`/`title`,
unknown keys, malformed hashes, invalid types and conflicting duplicate
infohashes. Identical duplicate records are skipped.

- `ih2`, `site`, `src`, `file_list` and `source_id` are recognized as
  source-only fields and deliberately omitted; they are not silently added to
  the signed MediaMesh schema.
- `category` maps to `tags`; `--category-map` accepts a JSON object mapping
  category names to a string or array of strings.
- `description`/`desc` and `description_file` upload UTF-8 text through the
  description media endpoint. `poster_file` uploads a signature-checked JPEG,
  PNG or WebP image. The API's returned signed refs are written to records.
- `--field-whitelist` accepts a JSON array of canonical output fields. Record
  identity fields (`ih`, `title`, `del`) cannot be removed.
- A plain `poster` URL remains metadata only; the publisher does not fetch it.
  No SHA-256 pseudo-URLs are written into the legacy poster field.

## Workflow

Create/select normal/meta lists; load a versioned profile with whitelist, source priorities/enabled flags, tag mapping,
category/default shard routing and budgets. Import batches, report accepted/duplicate/rejected/filtered,
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
