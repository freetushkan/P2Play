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

\`\`\`csv
infohash,name,size_bytes,created_unix,seeders,leechers,completed,scraped_date,published
00000649c83ac77c0f9675483db6f819b69ceb30,"Hard Quiz S08E26 1080p HDTV H264-CBFM",696894095,1695826211,2,0,0,1792956520,1697467411
00001f7eddd01e6f2d9d8c28695f3770bdcddfee,"[SubsPlease] Lv1 Maou to One Room Yuusha - 06 (1080p) [4D51D804].mkv",420876353,1693929977,15,2,8,1792926928,1693929982
\`\`\`

Mapping:

- \`infohash\` → lowercase \`ih\`; \`name\` → \`title\`; \`size_bytes\` → \`size\`.
- \`created_unix\` → canonical \`date\` by default. Keep \`published\` as
  provenance and allow a deliberate date-policy override. \`scraped_date\` is
  scrape time, not torrent creation time.
- \`seeders\`, \`leechers\` and \`completed\` are volatile/source metrics, not
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

### 2. RuTracker — operator-supplied XML dump

Input is a local XML dump supplied by the operator, not a tracker scraper.
The supplied shape is:

\`\`\`xml
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
\`\`\`

Mapping and behavior:

- Root \`torrent@id\` is the stable topic/source ID. Nested \`torrent@hash\`
  is the infohash; validate and lowercase it. Root \`size\` is bytes.
- Prefer root \`unixts\` as registration time. If missing, parse the exact
  \`registred_at\` attribute spelling/format using an explicitly configured
  timezone; never use the machine's implicit local timezone. Keep original
  text and source ID in local provenance.
- \`title\` → \`title\`; \`forum@id\` plus its text identify the forum/category.
  Add an explicit include-list of forum IDs and report filtered counts.
  Apply this filter before output. Forum-ID-to-tag/shard rules are configurable;
  do not infer IDs from display names.
- \`tracker_id\` is an integer, not a URL. Map ID 1 to
  \`http://bt.t-ru.org/ann?magnet\`; ID N > 1 to
  \`http://btN.t-ru.org/ann?magnet\`. Validate the number before constructing
  the address. Preserve other magnet/tracker provenance separately.
- \`content\` is markup, not trusted HTML. Convert it to bounded plain text for
  optional description upload; strip active markup and never execute or render it.
- \`dir\` elements may nest. Count leaf \`file\` elements recursively and sum
  sizes as a consistency check only; root \`size\` remains the torrent size.
  One-file torrents may put \`file\` directly under root \`torrent\`.
- \`del\` means the topic was deleted and should result in an infohash tombstone.
  The future \`dead="true|false"\` attribute is not available yet; do not invent it.
- \`old\` lists prior versions of the same topic. Preserve relationships in
  local provenance; do not create separate records or tombstones from them
  automatically.
- \`dup\` is currently absent/temporarily removed. If it reappears, treat it
  only as an untrusted suspicion signal, never proof that two torrents are
  interchangeable. Do not merge records based only on \`dup@p\`.

Add small sanitized fixtures for the real tag nesting, encoding and date
semantics before implementing the adapter. Use a bounded XML parser, disable
external entity/network resolution and reject malformed/oversized input.

**Deletion/shard invariant:** a tombstone must identify the shard where the
previous live record was published. Persist the last published \`ih → shard\`
mapping. Forum-to-shard rules must remain stable. If the previous shard is
unknown, report the deletion for review instead of blindly tombstoning
\`default\`.

### 3. Rutor — \`rutor.ls\` cache

Source: [releases.yourok.ru/torr/rutor.ls](http://releases.yourok.ru/torr/rutor.ls).
The file is a JSON array compressed with **raw DEFLATE**, not gzip, zlib-wrapped
DEFLATE, plain text or SQLite. Python uses:

\`\`\`python
payload = zlib.decompress(compressed_bytes, -15)
items = json.loads(payload)
\`\`\`

Accept either an operator-supplied local \`rutor.ls\` copied from the TorrServer
configuration directory or an explicitly enabled remote refresh. Remote mode
must use bounded timeouts/size limits and the same configured tunnel/egress
policy as publisher traffic. Enforce both compressed and decompressed size
limits before parsing JSON.

Field mapping from the supplied sample:

- \`Hash\` → lowercase \`ih\`; \`Magnet\` is a fallback source for a \`btih\`.
  If both are present, parse the magnet and reject the row if its infohash
  disagrees with \`Hash\`.
- \`Title\` → \`title\`; \`Name\` and \`Names\` are alternate-name metadata,
  retained locally unless later enrichment policy gives them an explicit use.
- \`CreateDate\` → canonical \`date\`, parsed as ISO-8601 with its supplied
  UTC offset. \`Year\` is optional enrichment, not a replacement for the date.
- \`Categories\` → source category and then configurable category-to-tag mapping.
- \`Size\` is human-readable (examples: \`706.17 MB\`, \`1.33 GB\`). Parse with
  decimal arithmetic and explicit unit factors established by representative
  fixtures; do not use binary floating point or silently guess MB/GB semantics.
- \`Magnet\` → source magnet; extract each repeated \`tr\` query parameter as
  a tracker URL. \`Tracker: "Rutor"\` is a source label, not an announce URL.
  \`Link\` is a relative source-page path/provenance; do not fetch arbitrary
  links from catalog rows.
- \`Seed\` and \`Peer\` are volatile swarm metrics; keep locally, not in the
  MediaMesh record.
- \`IMDBID\`, \`KPID\`, \`VideoQuality\` and \`AudioQuality\` are enrichment
  candidates. Do not add signed record fields without a schema/version decision.

The Rutor cache is the third/highest-priority layer in the supplied default
order. The sample does not establish how the cache is generated internally;
do not assume a scraping or update mechanism beyond the observed file format.

## Source order, precedence and merge rules

The supplied default order is:

1. torrents-csv-data — broad baseline / lowest priority;
2. RuTracker XML — higher priority;
3. Rutor \`rutor.ls\` — highest priority.

Make order configurable, and show the effective order in preview and logs.
Later sources win for the same normalized infohash. Merge per field: a
non-empty value from a higher-priority source replaces the lower-priority
value; a missing/empty optional field does not erase an earlier useful value.
For tags/trackers, use the selected higher-priority source's non-empty array
by default; optional union behavior can be added later. A higher-priority
tombstone suppresses a lower-priority live record; a later live record may
restore it. Conflicts within one source/revision must be reported with source
locations rather than resolved by arbitrary iteration order.

The final merged batch must contain at most one canonical row per infohash.
The publisher CLI skips identical duplicates and rejects conflicting records
for the same hash; it does not apply source priorities or upserts. The grabber
owns cross-source merge decisions and stores source provenance, timestamps,
tombstones and the last published shard mapping locally.

A source checkpoint advances only after its rows are normalized, merged,
validated and acknowledged as staged by the publisher. Failed/partial source
processing must be replayable and idempotent.

## Recent releases: RSS and Torznab

Recent tracker uploads are a recurring ingestion mode:

- Poll configured feeds/indexers at an operator-defined interval. RSS polling
  is periodic, not a push subscription.
- Prefer stable GUID/source IDs and normalized infohashes for deduplication.
- Persist per-source checkpoints and seen identifiers outside Git. Poll with
  an overlap window to catch delayed/re-ordered items; replay must be idempotent.
- Keep publication/update time separate from poll time.
- Handle pagination/limits, outages, transient errors, rate limits, malformed XML,
  missing dates and duplicates. Do not advance a checkpoint past unprocessed items.
- Specify what happens when a known infohash reappears with changed metadata.
  The append-only manifest does not imply an upsert policy.

### Reuse existing indexer infrastructure first

Do not write tracker-specific HTML scrapers by default. Evaluate:

1. **Jackett** as an external tracker/indexer aggregation service.
2. **Torznab** as the preferred standardized HTTP interface to such services.
3. Direct RSS 2.0 feeds as a simple adapter where a stable feed is available.

For Jackett/Torznab, start with per-indexer capabilities (`t=caps`) and category
mappings. Test search and recent-release polling against representative indexers;
capabilities vary. Compare Jackett's aggregate `all` indexer with per-indexer
feeds to retain control of categories, search modes and slow indexers.

Torznab returns RSS/XML-style item data. Map title, GUID, publication date,
category, enclosure/download reference and available Torznab attributes (such
as size and category IDs) into the normalized model. Treat results as untrusted
input: validate infohash/magnet and metadata locally.

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

- Use human-editable local configuration for sources, interval, categories, limits,
  paths and secrets (select exact config format during implementation).
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
- Cover XML date/timezone parsing, nested directories, tracker_id mapping,
  forum filters, deleted topics, old/dup handling, malformed XML/entity input
  and tombstone shard stability.
- Cover raw-DEFLATE corruption/truncation, expanded-size limits, JSON errors,
  Hash/Magnet mismatches, ISO offsets, size-unit parsing and repeated tracker
  query parameters.
- Test cross-source priority, missing-field retention, tombstones/live-row
  restoration, same-source conflict reporting and one canonical row per hash.
- Torznab fixtures for caps, search and recent items; direct RSS fixtures if used.
- Cover malformed XML, missing GUID/date/hash, invalid magnet, duplicate GUID/hash,
  changed metadata, pagination, retries and overlapping poll windows.
- Replaying a poll must be idempotent; dry-run must not touch publisher state.
- Integrate with a local/mock Torznab endpoint, not public trackers or private keys.
- End-to-end: publish a small signed release and verify normalized records and
  signed description/poster refs from a second node.
