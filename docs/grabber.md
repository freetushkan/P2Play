# Catalog grabber — design notes

Status: three concrete source formats and representative field samples are now known.
The Python grabber remains a separate operator tool; the publisher PR provides its downstream import API.

## Goals and placement

- Implement the grabber in **Python** as a separate operator/developer tool,
  not as part of the C++ daemon or a client runtime dependency.
- Use a clean pipeline: source adapter → parsed item → normalized catalog
  record → validation/deduplication → NDJSON / MediaMesh publishing.
- Adding a format should mean adding an adapter, not rewriting normalization
  or publisher code.
- Support one-shot imports and scheduled incremental refresh of recent releases.
- Keep credentials, API keys, cookies and personal data out of Git.

## Configurable parser templates

The parser must not be limited to hard-coded site adapters. Provide a user-editable
template system for JSON, pseudo-HTML and plain text sources. A template maps
source selectors/expressions to canonical fields (infohash/magnet, title, date,
size, file count, category, tracker/site, description, poster and source ID).
Templates should support JSON paths, CSS-like selectors for pseudo-HTML, and
regular expressions / named capture groups for line-oriented text, with explicit
fallbacks and validation.

Keep templates data-driven (YAML or JSON configuration), versioned and testable
against fixture files. Include preview/dry-run output showing extracted fields,
normalization errors and deduplication decisions before importing. Avoid arbitrary
code execution in templates; bound regex complexity, input size, nesting and
per-item processing time. Built-in adapters for known formats remain available
and may be overridden by local templates. The user will supply representative
JSON, pseudo-HTML and plain-text fixtures before adapter implementation.

## Supplied source formats

### 1. torrents-csv-data — base layer

Source repository: [heretic/torrents-csv-data](https://codeberg.org/heretic/torrents-csv-data/).

The dataset is a large CSV. **Do not download it for development or tests.**
The supplied header and sample establish these columns:

```csv
infohash,name,size_bytes,created_unix,seeders,leechers,completed,scraped_date,published
00000649c83ac77c0f9675483db6f819b69ceb30,"Hard Quiz S08E26 1080p HDTV H264-CBFM",696894095,1695826211,2,0,0,1792956520,1697467411
00001f7eddd01e6f2d9d8c28695f3770bdcddfee,"[SubsPlease] Lv1 Maou to One Room Yuusha - 06 (1080p) [4D51D804].mkv",420876353,1693929977,15,2,8,1792926928,1693929982
```

Mapping:

- `infohash` → lowercase `ih`; `name` → `title`; `size_bytes` → `size`.
- `created_unix` → canonical `date` by default. Keep `published` as
  provenance and allow a deliberate date-policy override. `scraped_date` is
  scrape time, not torrent creation time.
- `seeders`, `leechers` and `completed` are volatile/source metrics, not
  MediaMesh v1 fields. Keep them in local state/diagnostics; do not silently
  convert them to tags.

This is the first/lowest-priority input layer, a broad baseline catalog.
Persist the last successfully processed Git commit OID (and CSV blob OID when
available). Compare the local checkout's current commit before parsing; skip an
unchanged revision. Advance the checkpoint only after normalized records have
been acknowledged as staged by the publisher. Do not assume the CSV is
append-only or that a Git change contains only newly appended rows. Start with
a correct full-snapshot parse when the data blob changes; optimize with Git
diffs only after repository history and fixtures prove that row identity and
patch semantics are stable. Keep the checkout/local path configurable and avoid
fetching the large CSV blob merely to inspect commit metadata.

### 2. Rutor — `rutor.ls` cache

Source: [releases.yourok.ru/torr/rutor.ls](http://releases.yourok.ru/torr/rutor.ls).
The file is a JSON array compressed with **raw DEFLATE**, not gzip, zlib-wrapped
DEFLATE, plain text or SQLite. Python uses:

```python
payload = zlib.decompress(compressed_bytes, -15)
items = json.loads(payload)
```

Accept either an operator-supplied local `rutor.ls` copied from the TorrServer
configuration directory or an explicitly enabled remote refresh. Remote mode
must use bounded timeouts/size limits and the same configured tunnel/egress
policy as publisher traffic. Enforce both compressed and decompressed size
limits before parsing JSON.

Field mapping from the supplied sample:

- `Hash` → lowercase `ih`; `Magnet` is a fallback source for a `btih`.
  If both are present, parse the magnet and reject the row if its infohash
  disagrees with `Hash`.
- `Title` → `title`; `Name` and `Names` are alternate-name metadata,
  retained locally unless later enrichment policy gives them an explicit use.
- `CreateDate` → canonical `date`, parsed as ISO-8601 with its supplied
  UTC offset. `Year` is optional enrichment, not a replacement for the date.
- `Categories` → source category and then configurable category-to-tag mapping.
- `Size` is human-readable (examples: `706.17 MB`, `1.33 GB`). The
  default assumption is Windows-style binary units despite the source's MB/GB
  labels: 1 MB = 1,048,576 bytes (MiB), 1 GB = 1,073,741,824 bytes (GiB).
  This is an operational assumption, not verified against a source specification.
  Parse the numeric part with decimal arithmetic (never binary floating point),
  multiply by the configured factor and round to the nearest whole byte. Preserve
  the original size string in local provenance. Make the unit mode configurable
  (`binary` by default, `decimal` as an override) so it can be corrected without
  changing the adapter.
- `Magnet` → source magnet; extract each repeated `tr` query parameter as
  a tracker URL. `Tracker: "Rutor"` is a source label, not an announce URL.
  `Link` is a relative source-page path/provenance; do not fetch arbitrary
  links from catalog rows.
- `Seed` and `Peer` are volatile swarm metrics; keep locally, not in the
  MediaMesh record.
- `IMDBID`, `KPID`, `VideoQuality` and `AudioQuality` are enrichment
  candidates. Do not add signed record fields without a schema/version decision.

The Rutor cache is the second snapshot layer and takes precedence over the
CSV baseline. The sample does not establish how the cache is generated internally;
do not assume a scraping or update mechanism beyond the observed file format.

### 3. RuTracker — optional operator-supplied XML dump

RuTracker is **disabled unless explicitly enabled** in the grabber configuration. When enabled, its input is a local XML dump supplied by the operator, not a tracker scraper. If disabled, absent or unreadable, the pipeline continues without it.
The supplied shape is:

```xml
<torrent id="topic-id" registred_at="Y.m.d H:i:s"
         unixts="registration-unix" size="bytes">
  <title>Release title</title>
  <torrent hash="infohash" tracker_id="tracker-number"/>
  <forum id="forum-id">Forum / category name</forum>
  <del/>
  <content>Release description / markup</content>
  <dir name="directory">
    <file size="bytes" name="filename"/>
    <dir name="nested directory">
      <file size="bytes" name="nested filename"/>
    </dir>
  </dir>
  <old hash="old-infohash" time="old version date" unixts="old-version-unix">Old title</old>
</torrent>
```

Mapping and behavior:

- Root `torrent@id` is the stable topic/source ID. Nested `torrent@hash`
  is the infohash; validate and lowercase it. Root `size` is bytes.
- Prefer root `unixts` as registration time. If missing, parse
  `registred_at` using **UTC**. The timestamp is approximate metadata;
  hour-level precision is not critical. Never use the machine's implicit local
  timezone. Keep the original text and source ID in local provenance.
- `title` → `title`; `forum@id` plus its text identify the forum/category.
  Add an explicit include-list of forum IDs and report filtered counts.
  Apply this filter before output. Forum-ID-to-tag/shard rules are configurable;
  do not infer IDs from display names.
- `tracker_id` is an integer, not a URL. Map ID 1 to
  `http://bt.t-ru.org/ann?magnet`; ID N > 1 to
  `http://btN.t-ru.org/ann?magnet`. Validate the number before constructing
  the address. Preserve other magnet/tracker provenance separately.
- `content` is markup, not trusted HTML. Convert it to bounded plain text for
  optional description upload; strip active markup and never execute or render it.
- `dir` elements may nest. Count leaf `file` elements recursively and sum
  sizes as a consistency check only; root `size` remains the torrent size.
  One-file torrents may put `file` directly under root `torrent`.
- `del` means the topic was deleted and should result in an infohash tombstone.
  The future `dead="true|false"` attribute is not available yet; do not invent it.
- `old` lists prior versions of the same topic. Preserve relationships in
  local provenance; do not create separate records or tombstones from them
  automatically.
- `dup` is currently absent/temporarily removed. If it reappears, treat it
  only as an untrusted suspicion signal, never proof that two torrents are
  interchangeable. Do not merge records based only on `dup@p`.

Add small sanitized fixtures for the real tag nesting, encoding and date
semantics before implementing the adapter. Use a bounded XML parser, disable
external entity/network resolution and reject malformed/oversized input.

**Deletion/shard invariant:** a tombstone must identify the shard where the
previous live record was published. Persist the last published `ih → shard`
mapping. Forum-to-shard rules must remain stable. If the previous shard is
unknown, report the deletion for review instead of blindly tombstoning
`default`.

## Source order, precedence and merge rules

The pipeline has two ordered phases. The bulk/snapshot phase runs first; the
incremental RSS phase runs only after it has finished. Default precedence from
lowest to highest is:

1. **torrents-csv-data** — base layer, when configured and available;
2. **Rutor `rutor.ls`** — second layer, when configured and available;
3. **RuTracker XML** — optional third layer; disabled unless explicitly enabled;
4. **RSS / Torznab indexers through Jackett** — final and highest-priority layer.

Thus the normal setup uses CSV → Rutor → RSS. Enabling optional RuTracker adds
it between Rutor and RSS. An operator may reorder/disable sources, but the
effective order and each source's enabled/optional state must be printed in
preview and logs. RSS is not an alternative to snapshot sources: it is the
final incremental enrichment stage when polling is enabled.

Later/higher-priority sources win for the same normalized infohash. Merge per
field: a non-empty value from a higher-priority source replaces the lower
priority value; an absent/empty optional value does not erase useful earlier
metadata. For tags and trackers, the selected higher-priority non-empty array
wins by default; optional union behavior may be configured later. Same-priority
conflicts within one source/revision must be reported with both source
locations, not resolved by arbitrary iteration order. The final outgoing batch
contains at most one canonical row per infohash.

A higher-priority explicit tombstone suppresses a lower-priority live record;
a later higher-priority live record may restore it. The absence of an item from
a CSV snapshot, cache or RSS feed is **not** a deletion signal. RSS is a recent
items window: old entries naturally fall out of it. Only explicit source
deletion semantics (such as RuTracker `<del/>`) may generate tombstones, and
the grabber must know the prior published shard. If that shard is unknown,
quarantine the deletion for operator review.

The publisher CLI skips identical duplicate records and rejects conflicting
records for the same hash in one import. It does not perform cross-source
precedence or metadata upserts. The grabber owns source merge decisions,
stable source provenance, tombstones and the last published `ih → shard`
mapping. The append-only MediaMesh manifest does not by itself define a
metadata-update policy; changed metadata for an already published infohash
must use an explicit, tested publisher/upsert strategy, not an assumed one.

## Failure isolation and partial-source operation

**No individual source being skipped or failing may abort the whole grabber
run.** Every adapter (and every Jackett/Torznab indexer feed) is an independent
unit with its own timeout, checkpoint, status and error report.

- A disabled optional source, a missing optional file, an unchanged Git commit
  or an empty feed is recorded as `skipped` / `unchanged`; remaining sources
  continue.
- Download, decompression, parse, timeout, rate-limit or authentication failure
  is recorded against that source only. Continue with every remaining enabled
  source, including the final RSS stage. One source exception must not become
  a top-level abort.
- On failure, do not replace a source's last-known-good local snapshot with
  incomplete/empty data. Reuse that cached snapshot in the merge when one
  exists, mark it stale in the report and allow higher-priority healthy sources
  to override it. Without a cached snapshot, omit that source for this run.
- Persist failed rows/documents in a local quarantine/retry area with source ID
  and source location. Never silently drop malformed data. Do not advance a
  source checkpoint past unprocessed data unless rejected items are durably
  quarantined for retry.
- Each source has an independent checkpoint. A successful source's checkpoint
  may advance even if another source failed, but only after the final merged
  batch containing its accepted contribution is acknowledged as staged by the
  publisher. A failed/unavailable source retains its previous checkpoint.
- If publisher staging fails or the accepted count does not match, do not
  advance checkpoints for any source represented by that outgoing batch.
  Inspect publisher status before retrying; do not blindly repeat mutating POSTs.
- A partial run is useful: stage validated contributions from healthy sources
  plus last-known-good cached data, and report missing/stale sources. Never
  invent deletion or replacement data for a missing layer.
- Distinguish a degraded-but-successful run (at least one usable input and
  successful publisher staging) from a global failure (configuration/state
  corruption, no usable input or publisher staging failure). A source-local
  error alone must not prevent a healthy batch from being staged.

Show per source: enabled/disabled, fetched, parsed, accepted, filtered,
rejected/quarantined, duplicate, superseded, stale-cache-used, checkpoint
before/after and last error. Summarize the overall run as complete or degraded;
never report a failed source as successful.

## Final stage: recent releases via RSS / Torznab

After the CSV, Rutor and (when enabled) RuTracker snapshot layers have been
processed and merged, run the recent-release stage. Its results have the
highest source priority: a non-empty RSS/Torznab field may enrich or replace
the corresponding value from snapshot layers, while empty fields retain
earlier useful values. This stage is best-effort too and must not be skipped
because a preceding snapshot source failed.

- Poll configured feeds/indexers at an operator-defined interval. RSS polling
  is periodic, not a push subscription.
- Prefer normalized infohashes and stable GUID/source IDs for deduplication.
- Treat each configured Jackett indexer/feed independently: one failing or
  slow indexer must not prevent querying other indexers or publishing healthy
  results.
- Persist per-indexer checkpoints and seen identifiers outside Git. Use an
  overlap window to catch delayed/reordered items; replay must be idempotent.
- Keep source publication/update time separate from poll time.
- Handle pagination/limits, outages, transient errors, rate limits, malformed
  XML, missing dates/hashes and duplicates per feed. Do not advance a feed's
  checkpoint past unprocessed items.
- Make per-feed priority configurable. Same-priority conflicts for one hash
  go to quarantine rather than arbitrary first/last-row wins.
- A feed's omission of an old item is not a tombstone. Only an explicit,
  trusted deletion signal may remove a catalog record.
- Changed metadata for an already published hash still needs an explicit
  publisher/upsert policy; append-only history does not imply one.

### Reuse existing indexer infrastructure first

Do not write tracker-specific HTML scrapers by default. Evaluate:

1. **Jackett** as the external tracker/indexer aggregation service and operator-facing configuration point.
2. **Torznab** as the preferred standardized HTTP API exposed by Jackett for indexer capabilities, search and recent uploads.
3. Direct RSS 2.0 feeds as a separate adapter where a stable feed is available.

For Jackett/Torznab, first fetch/cache each indexer's capabilities (`t=caps`)
and category mappings. Then poll recent uploads per indexer. Compare Jackett's
aggregate `all` endpoint with per-indexer feeds; prefer per-indexer polling when
it provides better checkpointing, priority control, error isolation and visibility
of slow/unavailable indexers. Continue other feeds when one times out or returns
an error. Keep Jackett endpoints and API keys in local config/secrets, never Git.

Torznab returns RSS/XML-style item data. Map title, GUID, publication date,
category, enclosure/download reference and available Torznab attributes (such
as size and category IDs) into the normalized model. Resolve infohash from a
valid magnet or verified torrent metadata; do not assume GUID is an infohash.
Treat feed fields and URLs as untrusted. Bound response size, redirects, timeout,
item count and concurrency. Never fetch arbitrary enclosure/poster URLs without
an explicit source-adapter allowlist.

Jackett documents tracker-specific translation, multi-indexer search, standardized
Torznab/TorrentPotato responses and recent uploads similar to RSS. Torznab
defines capabilities and search APIs; optional modes/parameters vary by
implementation and indexer. References:

- [Jackett README and API usage](https://github.com/Jackett/Jackett#api-usage)
- [Jackett API wiki](https://github.com/Jackett/Jackett/wiki/Jackett-API)
- [Torznab / Newznab API specification](https://torznab.github.io/spec-1.3-draft/external/newznab/api.html)

Jackett remains an external optional service, not a vendored dependency. Configure
endpoint and API key locally; redact secrets from logs and diagnostic output.

## Normalization and publishing

- Normalize hashes, title, size, file count, dates, tags and tracker URLs.
- Optional enrichment: missing metadata via BEP 9; poster WebP thumbnails
  (initial target 300 px / 30 KiB); strip markup from descriptions (initial 2 KiB).
- Preserve safe provenance (source/indexer/feed and GUID) for resuming polls and
  diagnosing duplicates; never publish tracker credentials.
- Validate description/poster MIME, size and content; upload through existing
  publisher media endpoints and store the returned signed media refs. Never fetch
  arbitrary remote media URLs while serving a catalog record.
- Validate a batch before release where possible; report per-item failures and
  avoid partially publishing a malformed import.
- Specify metadata update/upsert behavior before implementation.

## Configuration and operations

- Use human-editable local configuration for sources, source order, optional
  RuTracker enablement, forum include IDs, category/tag mapping, RuTracker
  fallback timezone (UTC), Rutor size-unit mode (`binary` by default, optional
  `decimal` override), shard rules, interval, limits, paths and secrets
  (select exact config format during implementation).
- Support dry-run/preview, one-shot import and repeatable polling. Schedule via
  systemd timer; run through the same SOCKS5 or WireGuard/netns tunnel as publisher.
- Persist checkpoints, deduplication state and retry scheduling.
- Bound timeouts, result count, download size and concurrency; respect rate limits.
- Log source status without keys, cookies, credential-bearing URLs or private data.
- Report fetched, parsed, rejected, duplicate, updated, media-uploaded and published
  counts.

## Tests and acceptance

- Small sanitized fixtures for the CSV header/rows, RuTracker XML (nested and
  single-file layouts, forum filter and deletion), and raw-DEFLATE Rutor JSON.
  Never commit production dumps, full CSV snapshots or large cache files.
- Cover CSV quoting and Git commit checkpoint advancement only after publisher
  staging; unchanged commits must skip, failed imports must replay safely.
- Cover XML date parsing with `unixts` preference and UTC fallback for
  `registred_at`, nested directories, tracker_id mapping, forum filters,
  deleted topics, old/dup handling, malformed XML/entity input and tombstone
  shard stability.
- Cover raw-DEFLATE corruption/truncation, expanded-size limits, JSON errors,
  Hash/Magnet mismatches, ISO offsets, binary Rutor size conversion (including
  fractional values and nearest-byte rounding), decimal override, preservation
  of the original size string and repeated tracker query parameters.
- Test cross-source priority, missing-field retention, tombstones/live-row
  restoration, same-source conflict reporting and one canonical row per hash.
- Torznab fixtures for caps, category mapping and recent items, including one
  slow/failing indexer alongside a healthy one; direct RSS fixtures if used.
- Fault-isolation tests: each source can be disabled, absent, malformed, offline
  or rate-limited without suppressing subsequent sources; healthy sources still
  stage and advance only their own checkpoints.
- Stale-cache tests: a failed high-priority source reuses its last-known-good
  snapshot rather than allowing lower-priority metadata to overwrite it.
- Cover malformed XML, missing GUID/date/hash, invalid magnet, duplicate GUID/hash,
  changed metadata, pagination, retries and overlapping poll windows.
- Replaying a poll must be idempotent; dry-run must not touch publisher state.
- Integrate with a local/mock Torznab endpoint, not public trackers or private keys.
- End-to-end: publish a small signed release and verify normalized records and
  signed description/poster refs from a second node.
