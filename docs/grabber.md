# Catalog grabber — design and operator workflow

Status: the grabber foundation and configurable source pipeline are implemented.
Built-in adapters cover local CSV, Rutor raw-DEFLATE, RuTracker XML/XZ snapshots,
per-indexer Torznab/Jackett and direct RSS. A separate JSON template engine supports
JSON paths, bounded CSS-like selectors for pseudo-HTML and named regex groups for
text sources. The grabber remains review-first: transactional SQLite state, source
priority, quarantine, editable full/delta NDJSON, manual overrides/suppressions and
explicit acknowledgement before checkpoints advance.

The operator workflow is deliberately review-first: **run → inspect/edit → publish
manually with the existing publisher CLI → acknowledge**. The grabber never calls
the publisher API or CLI and never publishes automatically.

## Compact source policies (pending PR/CI)

See `docs/catalog-layout.md` for the opt-in RuTracker media profile, deleted
inventory sidecar, optional provenance extraction, exclusive CSV title groups,
policy-aware checkpoints and the lists-versus-shards decision. These policies do
not change signed MediaMesh records or automatically publish/split catalogs.

## Goals and placement

- Implement the grabber in **Python** as a separate operator/developer tool,
  not as part of the C++ daemon or a client runtime dependency.
- Use a clean pipeline: source adapter → parsed item → normalized catalog
  record → validation/deduplication → editable NDJSON export.
- The grabber must **not** send data directly to the publisher, call its API or
  launch the publisher CLI. Manual review/editing of the generated NDJSON is a
  required stage; publication is a separate explicit operator action.
- Adding a format should mean adding an adapter, not rewriting normalization
  or publisher code.
- Support one-shot imports and scheduled incremental refresh of recent releases.
- Every source is optional and independently enabled/disabled. There is no
  mandatory base source: a user may run with any subset, including no source
  enabled (a reported no-op with no checkpoint advancement).
- Keep credentials, API keys, cookies and personal data out of Git.

## Current implementation

The current implementation is `tools/grabber.py` plus the standard-library module
`tools/grabber_lib.py`. It uses JSON profile version 1 and local SQLite state.
The example profile is `tools/grabber-profile.example.json`; sources are disabled
by default and must be explicitly enabled/configured by the operator.

Implemented adapters in this first slice:

All built-in source types are selected through `SOURCE_ADAPTERS` in
`tools/grabber_adapters.py`; CSV and Rutor are registered through compatibility
wrappers around the first working handlers, and new/custom parser types are added
there without changing the SQLite/export pipeline.

- **torrents-csv-data:** reads an existing local Git checkout or a configured local
  CSV file; it never clones/fetches the large dataset. For a Git checkout it records
  HEAD, the HEAD blob OID and the worktree hash. A changed worktree is streamed and
  diffed by infohash. Title regex include/exclude filters, `any`/`all` matching
  and configurable `created_unix`/`published` date policy are supported.
  Seeder/leecher/completed and scrape/published metrics stay in local provenance.
- **Rutor:** reads a local `rutor.ls` raw-DEFLATE cache. A remote URL is used only
  when `remote_enabled: true` is explicitly configured. Compressed/decompressed
  sizes, JSON depth and record count are bounded. Hash/Magnet consistency,
  ISO-8601 offset date, binary/decimal size conversion and repeated tracker URLs
  are handled. Trackers containing obvious passkeys or credential-bearing URLs
  are rejected. Source-only fields and volatile metrics remain local provenance.
- **RuTracker XML/XZ:** reads an operator-supplied `.xml` or `.xml.xz` snapshot.
  XZ decompression is streaming, uses a decoder memory cap and a separate expanded
  byte limit. DTD/entities, corrupt/truncated XZ and oversized XML are rejected.
  Topic/torrent/forum/old/dup/del nesting, `unixts` preference, UTC fallback for
  `registred_at`, tracker ID mapping, recursive file counts and raw forum inventory
  are supported. `old` and `dup` remain local provenance. Only explicit `<del/>`
  creates a tombstone; it must use the shard of a previously staged record or go
  to quarantine.
- **Jackett/Torznab:** each indexer/feed is an independent source ID with its own
  endpoint, priority, capabilities map, poll limits, overlap window and checkpoint.
  The adapter reads caps and RSS-style search pages, resolves hashes only from
  magnet/infohash metadata (never by fetching arbitrary enclosure URLs), records
  volatile seed/peer metrics only as local provenance, quarantines malformed items
  and refuses HTTP redirects so API keys are never forwarded to another host.
- **Direct RSS:** uses the same bounded RSS/XML item parser without Torznab caps;
  an item needs a verified infohash in a magnet/GUID/attribute.
- **JSON parser templates:** `tools/grabber_templates.py` supports JSON paths,
  bounded CSS-like selectors for HTML fragments and named regex groups for
  line-oriented text. Templates live under the profile's top-level `templates`
  object and sources refer to them by name. Templates cannot execute Python/code.

The initial slice also provides:

- per-source savepoints and failure isolation; a failed parse cannot leave a partial
  source batch in the merge, and last-known-good observations remain available;
- priority-based whole-field merge, same-priority conflict quarantine and stable
  shard routing for already staged hashes;
- raw category inventory before category mapping;
- local `quarantine/<run-id>/` files, bounded per-source error reports and durable
  manual suppressions/overrides;
- full/delta hashes and a pending-run manifest. Checkpoints advance only after an
  explicit `ack` tied to the file and publisher accepted count. A changed full
  file is authoritative and requires `--replace`; independently edited delta files
  are rejected during acknowledgement;
- no-op behaviour when no source is enabled/configured; no checkpoint is advanced.

The source list and parser templates are stored together in the operator's JSON
profile. For example, the checked-in `tools/grabber-profile.example.json` contains
disabled examples for RuTracker `.xml.xz`, one Torznab indexer, direct RSS and a
generic JSON template source, plus JSON/HTML/text template definitions. Copy the
profile to a private local file, configure each endpoint/path and secret there, and
enable only the sources you intend to query.

A Torznab source uses a unique ID per indexer, an endpoint URL without embedded API
credentials and a separate `api_key` setting. The adapter requests `t=caps`, then
pages `t=search` results with configured `page_size`, `max_pages` and
`max_items`. Per-indexer `last_pubdate` and capability hash are checkpointed only
after `ack`. The overlap window deliberately replays recent items; deduplication
and source observations make replay idempotent. Direct RSS uses `type: "rss"` and
`fetch_caps: false`.

RuTracker source `path` may point at `rutracker-YYYYMMDD.xml` or
`rutracker-YYYYMMDD.xml.xz`. The XML is read and decompressed as a stream; the
whole dump is never loaded into memory. Concatenated XZ streams and valid XZ
stream padding are supported. Non-zero trailing bytes, truncated streams and
invalid padding are rejected. The limits are independent:

- `max_compressed_bytes`: default 8 GiB; configurable up to the hard 16 GiB cap.
- `max_expanded_bytes`: default 64 GiB; configurable up to the hard 128 GiB cap.
- `xz_memory_limit`: default 128 MiB; allowed range 16 MiB–2 GiB. This limits
  the XZ decoder dictionary memory, not the total XML size. If the decoder reports
  that its memory limit was exceeded, increase this value (for example to
  `1207959552` for 1152 MiB) only if the machine has enough available RAM.
- `max_topics`: default 10,000,000 and hard cap 50,000,000. It counts every
  RuTracker topic scanned, including topics later excluded by forum selection.

`forum_include_ids` is applied before detailed topic normalization. Topics outside
the selected forums are still scanned in the XML stream and counted against
`max_topics`, but their file lists, descriptions and historical duplicate data are
not traversed or staged. An empty array means **all forums**, not none.

The final merged catalog remains separately capped at 1,000,000 records and
256 MiB of NDJSON import/export data. Increasing the XML input limits does not
remove those downstream safety limits; use forum selection and the grabber's
normal source filters to keep the published catalog within them.

The local state DB, work outputs, quarantine and credentials belong outside Git.

## Independent source inventory (no merge / no publishing)

Use `grabber.py inventory` before designing lists or shards. This mode processes
each enabled source into a dedicated inventory SQLite database, keeps observations
keyed by `(source_id, infohash)`, stores that source's original category labels and
IDs, and writes per-source category/size reports plus pairwise infohash intersections.
It does **not** create `catalog-full.ndjson`, `catalog-delta.ndjson`, publisher
requests, signed manifests or shard assignments.

Example:

```sh
python3 -I tools/grabber.py inventory \
  --profile /path/to/grabber-profile.json \
  --report /path/to/inventory/inventory-report.json \
  --state /path/to/inventory/inventory.sqlite3 \
  --categories-dir /path/to/inventory/categories
```

To process the heavy RuTracker snapshot independently first, then add other sources
to the same inventory database:

```sh
python3 -I tools/grabber.py inventory --source rutracker \
  --profile /path/to/grabber-profile.json \
  --report /path/to/inventory/inventory-report.json \
  --state /path/to/inventory/inventory.sqlite3

python3 -I tools/grabber.py inventory --source rutor \
  --profile /path/to/grabber-profile.json \
  --report /path/to/inventory/inventory-report.json \
  --state /path/to/inventory/inventory.sqlite3

python3 -I tools/grabber.py inventory --source torrents-csv-data \
  --profile /path/to/grabber-profile.json \
  --report /path/to/inventory/inventory-report.json \
  --state /path/to/inventory/inventory.sqlite3
```

Repeat `--source ID` to select more than one enabled source. Without `--source`,
all enabled sources are processed. A disabled source is never activated by the CLI;
enable it in the local profile first. The inventory DB must be separate from the
normal publisher/grabber state DB.

Each source is committed independently. A failed refresh retains its last successful
observations and marks `stale_cache_used`; another source can still be inventoried.
Snapshot adapters (CSV, Rutor, RuTracker, templates) replace their prior source
inventory only after a clean scan. If records were rejected or conflicted, the old
observations are retained and valid new rows are added, the pass is marked degraded,
and its source checkpoint is deferred so the same snapshot can be retried after the
cause is corrected. RSS/Torznab are incremental, bounded feed windows; they are not
complete historical snapshots. Their pagination, overlap and `max_items` settings
define coverage.

Reports contain, per source: unique infohashes, active and deleted entries, records
with/without size, sum of torrent payload sizes, normalized metadata bytes, records
with/without categories and a category breakdown (records, deleted topics, known-size
records, unknown-size records and summed payload size). Taxonomy categories exposed
by a source but not used by an item remain in the report with zero counts. Pairwise
intersections count shared live infohashes without merging or choosing a winner.

### RuTracker category-group summary (alongside native forums)

A RuTracker source may define `category_groups`, mapping a group name to a
non-empty array of explicit forum IDs. Use the curated starter profile in
`tools/profiles/rutracker-media.example.json`, or define your own rules:

```json
"category_groups": {
  "Movie": ["100", "101"],
  "Music": ["200"]
}
```

These IDs illustrate syntax only; they are not recommended real forum assignments.
A forum ID may belong to only one group. Group membership is determined from stored
`provenance.forum_id`, not forum display names or publisher `category_map`.

The source JSON report and its per-source `categories/<id>.json` now contain
`category_group_summary`, with `groups`, `ungrouped` and a `basis` description.
The HTML report uses **RuTracker category groups** as the primary table, with
first column **Category**, an **Included forums** count, and **Other** last. The
original forum table remains available under collapsed **Native forum details**.
The old **Relative count** bar (count / largest category count) was removed; it
was neither a percentage of the catalog nor source coverage. Native `categories`, forum IDs/names and source metrics
are unchanged; no grouping is written back to stored observations.

Each group reports `records` (active hashes), `deleted_topics`,
`records_with_size`, `records_without_size`, `torrent_size_bytes` and its configured
forum IDs in `category_ids`. Deleted hashes contribute only to `deleted_topics`,
never active counts or payload bytes. Zero is a known size. Empty configured groups
remain visible. Unknown/missing forum IDs and IDs without a matching rule go to
`ungrouped` (displayed as **Other**), which is separate from configured groups even if a group is literally
named "Other". Without rules, all stored records are ungrouped.

Included forums counts configured IDs for each group (including empty forums);
Other counts distinct observed unmatched forum IDs. Missing IDs contribute records
to Other, but do not invent an additional forum.

The summary is streamed from SQLite using **current profile rules** at report
creation. It also works for `unchanged`, disabled and unselected cached sources;
it does not need the original XML. Missing cached provenance is not guessed from
labels: it goes to ungrouped. For a report-only regrouping with no XML parse, leave
RuTracker disabled and run inventory with the existing DB (or refresh another
source explicitly). An enabled RuTracker source can still rescan after a policy
change because its checkpoint fingerprint has changed.

`forum_include_ids` remains an independent input filter: `[]` scans all forums;
`category_groups` does not itself filter the input. A summary over cached data
reflects the previous scan's filters/freshness, not a newly applied include-list.
Groups plus ungrouped partition the stored source's active and deleted hashes;
this is a per-source view, not cross-source deduplication or publication.

### Directional coverage matrix

The HTML report shows an N × N **Source coverage matrix** (3 × 3 for three
inventoried sources). JSON exposes `source_coverage_matrix.sources` and `rows`,
with each row's `percent_by_source` keyed by column source ID.
Cell A,B = 100 × shared live hashes(A,B) / active hashes(A). Thus A,B and B,A
can differ. Diagonal is 100% for nonempty sources, empty row percentages are null
(HTML —), and absent intersections are zero. Deletions never contribute.

The matrix includes configured sources with cached live observations, plus sources
participating in the current refresh even if empty. Disabled/unselected empty
example sources do not produce extra rows/columns. Cached disabled data remains
visible: consult source status for freshness. Omit `--source` to refresh all enabled
sources; selection affects refresh, not cached comparisons. Existing pairwise counts
and legacy JSON `source_coverage` union metrics remain for compatibility, but the
union table is no longer shown in HTML. Removed-from-profile inventories are not
matrix columns (legacy union metrics still use the whole database).

### Inventory catalog size estimates

Source metrics and each RuTracker grouped row now include `catalog_ndjson_bytes`
and `catalog_zlib_estimate_bytes`. NDJSON counts live normalized record JSON in
infohash order plus one newline per row. Compression is streamed with standard
Python zlib level 6, splitting on record boundaries at 8 MiB, independently for
each group/source. Tombstones and local provenance are excluded. Empty sets report
zero bytes. Group compressed totals need not sum to whole-source compression.

These are **catalog payload estimates**, NOT the final SQLite database size or
exact MediaMesh published bytes. MediaMesh uses RFC1950/1951 zlib segments, but its
portable C++ `zlibDeflate` encoder uses fixed codes/greedy LZ77, not Python zlib's
level-6 encoder. Publisher routing/tags/refs, JSON serialization, segment ordering,
manifest, torrent metadata, media packs and client index/storage overhead can all
change final size. Actual final size requires publishing and measuring that release
and client DB. `catalog_size_basis` documents this distinction in JSON.

Category models differ by source:
- RuTracker: forum ID/name; an empty `forum_include_ids` scans all forums.
- Rutor: the original `Categories` values.
- Jackett/Torznab: capabilities category IDs/names and per-item category attributes.
- RSS: whatever category labels the feed supplies.
- Templates: categories selected by the configured template.
- torrents-csv-data: its current CSV schema has no explicit category column, so
  inventory can measure rows/sizes but cannot report native categories for it.

For torrents-csv-data, the optional `inventory_categories` object can define
heuristic title groups independently of the CSV source's native fields. The example
profile splits the media title patterns into `Video` (container/scene-release patterns)
and `Music` (audio-format/codec patterns), with `inventory_category_mode: "first"`
so Video takes precedence over audio codecs in a video title. The existing `filters.title_regex` still
decides which rows enter the media inventory; `inventory_categories` only labels
accepted rows for analysis. In legacy/default `all` mode a title can match both groups. These labels are not native
CSV categories and are never copied into publisher tags. They help estimate the
audio/video split, not decide the final taxonomy. Rutor's native `Anime` category
remains independently visible and is not merged into Movie/Series.

The HTML companion report is written next to the JSON report using the same basename
with a `.html` extension. It contains source overview tables, category counts and
payload-size totals, per-source warnings/filter counters and pairwise hash intersections.
It is standalone and uses no remote scripts or assets. Category counts and byte totals
can overlap when the same infohash belongs to multiple categories.

The `publisher.category_map` and `publisher.shard_policy` settings are not used
to merge, rename or allocate inventory categories. Keep category mapping empty during
inventory so the report reflects the source's native taxonomy. No ACK is required:
inventory checkpoints are local to `inventory.sqlite3` and never affect publisher
checkpoints or signed catalog state.

## Configurable parser templates

The parser is not limited to hard-coded site adapters. The built-in user-editable
template system supports JSON, pseudo-HTML and plain text sources. A template maps
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
and may be overridden by local templates. Sanitized JSON, pseudo-HTML and plain-text
fixtures are included in the automated template tests.

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

#### CSV title filters

The first source supports configurable regular-expression filters applied to
its `name` / normalized title. The filter profile has:

- `include`: regex patterns for video filename extensions and release/codec
  markers (for example `.mkv`, `.mp4`, `DVDRip`, `BDRip`, `WEBRip`,
  `WEB-DL`, `HDTV`, `AVC`, `HEVC`, `H.264`, `H.265`, `x264`,
  `x265`, `XviD`), plus audio formats and codecs (`FLAC`, `ALAC`, `APE`,
  `WavPack`, `WAV`, `AIFF`, `DSD`, `DSF`, `DFF`, `SACD`, `MP3`, `AAC`,
  `M4A`, `OPUS`, `OGG`, `Vorbis`, `WMA`, `AC3`, `DTS`, `MQA`). ISO
  disc images are intentionally not part of the example whitelist;
- `mode: "any"` (default) accepts a title when at least one include pattern
  matches; `mode: "all"` requires every include pattern to match;
- `exclude` drops a title when any exclude pattern matches;
- empty include/exclude arrays disable that side of the filter.

Patterns are configurable rather than hardwired to one fixed release taxonomy.
Use Python-compatible regular expressions, compile and validate them before
processing, apply bounded input/time limits, and report the matched/rejected
counts and the pattern responsible. The sample profile contains a starter
pattern set, not a final whitelist; tune it against the real CSV data.

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

Small sanitized XML/XZ fixtures now cover the observed tag nesting and date
semantics. The adapter uses a streaming bounded parser, rejects DTD/entities and
limits both compressed input and expanded XML size.

**Deletion/shard invariant:** a tombstone must identify the shard where the
previous live record was published. Persist the last published `ih → shard`
mapping. Forum-to-shard rules must remain stable. If the previous shard is
unknown, report the deletion for review instead of blindly tombstoning
`default`.

## Category inventory and manual editing

The grabber always writes a complete authoritative `catalog-full.ndjson`,
a derived `catalog-delta.ndjson`, a report and a pending-run manifest. It never
publishes automatically. The full file is the human review surface; the delta
is derived from the unedited full file and last acknowledged publisher state.

- If the full file SHA-256 is unchanged, publish only the delta through the
  normal append/release path. Omission from a delta means "unchanged", not delete.
- If the full file SHA-256 changed after export, do not publish the delta.
  Treat the edited file as the complete desired catalog and use publisher
  `--replace`; every omitted infohash is deleted.
- `--replace` is also available deliberately for cleanup, repair or rebuilding.
  An empty file is a valid request to clear the catalog.
- Replacement preserves the list public key and commits one fresh signed
  snapshot atomically. It is not ordinary compaction.

No network request to the publishing API is made by the grabber.

Suggested operator flow:

```sh
python3 tools/grabber.py run --profile publisher-profile.json \
  --output work/catalog-full.ndjson --delta work/catalog-delta.ndjson \
  --report work/grabber-report.json --dump-categories work/categories

# Unedited full file: publish the derived delta.
tools/p2play publish --profile publisher-profile.json \
  --import work/catalog-delta.ndjson --list <64-hex-public-key> --dry-run
tools/p2play publish --profile publisher-profile.json \
  --import work/catalog-delta.ndjson --list <64-hex-public-key> --release

# Edited full file: replace from the complete edited catalog.
tools/p2play publish --profile publisher-profile.json \
  --import work/catalog-full.ndjson --list <64-hex-public-key> --dry-run
tools/p2play publish --profile publisher-profile.json \
  --import work/catalog-full.ndjson --list <64-hex-public-key> --replace
```

The implemented CLI is invoked directly from the checkout:

```sh
# Export an editable full catalog, its derived delta, report and pending-run manifest.
python3 -I tools/grabber.py run --profile tools/grabber-profile.example.json \
  --output work/catalog-full.ndjson --delta work/catalog-delta.ndjson \
  --report work/grabber-report.json --dump-categories work/categories

# Review the report/full file. Then explicitly stage the delta or full replacement
# with tools/p2play publish. The grabber itself never calls the publisher.

# Confirm publisher staging; --file always points to catalog-full.ndjson.
python3 -I tools/grabber.py ack --state work/grabber.sqlite3 \
  --run-id <run-id> --file work/catalog-full.ndjson \
  --accepted-count <publisher-staged-record-count> --mode auto

# Discard a reviewed run without advancing source checkpoints.
python3 -I tools/grabber.py discard --state work/grabber.sqlite3 --run-id <run-id>

# Inspect or clear persistent manual decisions (only when no run is pending).
python3 -I tools/grabber.py manual list --state work/grabber.sqlite3
```

By default the state DB is `grabber.sqlite3` beside the report; pass `--state`
to choose another path. `ack --mode auto` chooses delta when the full file is
byte-for-byte unchanged, otherwise it requires an authoritative publisher
replacement. A manually edited delta never qualifies as a normal delta acknowledgement.


When `--dump-categories DIR` is supplied, collect unique **raw source
categories** in parallel with parsing, before category mapping, source-priority
merging and title/forum filters. Write a separate UTF-8 text file for each
source (and each Jackett indexer where relevant), sorted deterministically,
one unique category per line. Where a source exposes stable IDs, preserve the
ID alongside its display name (for example, RuTracker forum ID and title,
Torznab category ID and label). Do not merge or rename category values in these
inventory files. CSV currently has no category column in the supplied sample;
do not invent categories for it. The report should state which source produced
each inventory file and how many unique values it contains.

This mode is for discovering the real taxonomy before making policy decisions.
Do not hardcode category unions/merges yet. Keep publisher `category_map`
empty/identity by default; after inspecting category inventories, the operator
can deliberately add mappings (including merging multiple source categories
into one tag) to the profile. Category inventory mode must not modify publisher
state or advance source checkpoints by itself.

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
precedence. MediaMesh replays record segments in signed manifest order: a later
live row with the same infohash replaces the earlier row, and a `del` row
removes it. This is a **whole-record replacement**, not a partial JSON patch:
a delta row must contain the complete canonical record, because omitted fields
become empty/default rather than being inherited. The grabber owns source merge,
stable provenance, explicit tombstones and the last staged `ih → shard` mapping.

Keep the existing shard for an infohash during ordinary refresh. Moving an item
between shards needs a tombstone in the old shard and a live row in the new one.
The current publisher groups new segments by shard, so the initial grabber must
not combine both halves into one release; use a reviewed two-release migration
or leave the shard unchanged.


## Incremental state, delta export and MediaMesh

Do not confuse three different kinds of incrementality:

1. **Source acquisition.** Some sources provide a versioned snapshot, while
   RSS/Torznab provides a recent-items window that can be polled by cursor.
2. **Grabber processing and export.** The grabber keeps local source/merge state
   and emits only the records that differ from the last successfully staged
   catalog state.
3. **MediaMesh distribution.** A regular release appends immutable record
   segment files to the signed manifest. A subscribed node should reuse
   previously verified files and fetch only new/missing payload.

A full source scan does not imply a full catalog upload. A changed CSV or Rutor
snapshot may still need a complete parse to discover all additions/updates, but
the resulting outgoing NDJSON should normally be a small delta.

### Bootstrap and subsequent refreshes

- **Bootstrap:** export the complete `catalog-full.ndjson` baseline.
- **Every refresh:** write the complete canonical `catalog-full.ndjson` and
  derive `catalog-delta.ndjson` against the last acknowledged publisher state.
- **Untouched full file:** publish only the delta. Changed rows are complete
  record replacements; trusted source deletions become tombstones.
- **Edited full file:** any hash change makes the file authoritative. Publish
  the whole file with `--replace`; omitted rows are deleted.
- **Forced rebuild:** use `--replace` even without editing for cleanup/repair.
- Compare a stable canonical fingerprint per infohash. Exclude poll time,
  scrape time, seed/leech/completed counters, local paths, source debug data and
  other volatile/provenance-only values from the signed-record fingerprint.
  A source poll that changes only volatile counters must not create a release.
- A changed row in a delta is a **complete replacement record**, not a patch.
  Include its current title, shard, date and every currently known non-empty
  canonical field. Preserve fields that remain valid from lower-priority
  observations or the last staged row. Do not rely on the publisher to merge
  partial fields.
- An unchanged or absent item in a snapshot is not a tombstone. Only explicit,
  trusted deletion semantics (currently RuTracker `<del/>`) may create
  `{"del":"<ih>","shard":"<previous-shard>"}`. The old shard must come from
  local last-staged state, never from a new/default routing guess.
- Keep shard stable for existing infohashes in the first implementation.
  Cross-shard migration is a separate reviewed operation: release the old-shard
  tombstone first, then release the new-shard live row. Do not emit both sides
  in one ordinary delta until ordering is explicitly implemented and tested.

Provide a full export/repair mode for rebuilding a local state database, but
make delta export the default once a baseline has been acknowledged. Every run
still produces an editable NDJSON file, a JSON report and a pending-run
manifest. The grabber never calls the publisher API or CLI itself.

### Per-source update strategy

| Source | Change detector | Work when changed |
| --- | --- | --- |
| torrents-csv-data | Git commit OID plus the `torrents.csv` blob OID | Skip an unchanged blob. When the blob changes, stream-parse the complete snapshot and diff rows by infohash. Do not use line-only Git diffs or assume append-only history until repository semantics are proven. |
| Rutor `rutor.ls` | SHA-256 of compressed local/downloaded file | Skip identical bytes. A changed file is still a full raw-DEFLATE/JSON snapshot; parse it and diff by infohash. |
| RuTracker XML | SHA-256 of the operator-supplied dump | Skip identical bytes. A changed dump is parsed as a snapshot; `<del/>` is an explicit event, omission is not. |
| Each Jackett/Torznab indexer | Per-indexer cursor plus overlap window and stable GUID/infohash set | Poll recent items independently. Replay the overlap idempotently, catch delayed items and do not treat feed-window expiry as deletion. |

Snapshot adapters must retain last-known source observations for entries omitted
from a later snapshot; omission alone does not revoke an earlier observation.
Mark those observations as not-seen-in-latest-snapshot for diagnostics. Only a
source's explicit deletion event or a deliberate operator cleanup may remove
its contribution. This prevents a truncated feed or partial snapshot from
silently deleting catalog data.

### Local transactional state and manual acknowledgement

Use a local SQLite database (standard-library `sqlite3`; never commit it to Git)
for source checkpoints, per-source observations/fingerprints, merged canonical
rows, last acknowledged staged rows including `ih → shard`, and pending runs.
SQLite is preferable to loading a multi-million-row catalog into Python objects
or maintaining several loosely coupled JSON state files. Keep raw inputs and
quarantine data as bounded files outside the database.

A run builds candidate source changes without advancing committed checkpoints.
Its pending-run manifest records candidate revisions/cursors, original full-file
SHA-256, delta SHA-256, output paths and target list. Refuse another run while
an unresolved run exists.

After review, compare the full-file hash with the pending-run manifest. If
unchanged, dry-run and explicitly publish the delta. If changed, dry-run and
publish the full file with `--replace`. Acknowledge only after the matching
publisher operation succeeds; failed publication must never advance source
checkpoints. A provisional interface is:

```sh
python3 -I tools/grabber.py ack --state work/grabber.sqlite3 \\
  --run-id <id> --file work/catalog-full.ndjson \\
  --accepted-count <publisher-staged-count> --mode auto
```

Acknowledgement verifies run ID, final full-file hash, publication mode and
publisher result, then updates last-published rows and successful source
checkpoints in one SQLite transaction. It does not contact the publisher.
Manual omissions must persist as local suppressions and manual edits/additions
as local overrides after source merging; otherwise the next source scan could
reintroduce intentionally removed entries. Provide explicit list/clear controls
for these decisions. A zero-row delta may be acknowledged as a reviewed no-op.

### Why this fits MediaMesh

A non-snapshot `release` carries existing signed segment descriptors forward
and adds new immutable segment file(s) for the new records, grouped by shard.
Existing segment bytes are not rewritten. The version pointer and torrent
metadata still change on every sequence, but the payload can remain incremental.

The subscriber must also preserve local bytes across version changes. The
`meshFetch` path now preloads selected, present record segments from the
currently held version into the new torrent working directory, but only after
checking their size and SHA-256 against the previous signed descriptors.
Libtorrent checks those copied bytes against the new v2 torrent before treating
them as complete; the new signed manifest is still verified independently.
Files that are new, absent, unselected, or fail verification are downloaded
(or remain unavailable) through the normal selective path. This does not change
the MediaMesh v1 wire format.

A normal `snapshot`/compaction replays the currently published live records;
it does not accept a replacement file. Publisher `--replace` instead builds a
fresh snapshot from the authoritative full NDJSON, deleting absent records.
Both create new immutable segment names and may require subscribers to fetch
selected shards again. Routine RSS updates remain append-only delta releases.

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
- Each source has an independent checkpoint. Exporting NDJSON does **not**
  advance any checkpoint: the operator may still edit the file or decide not
  to publish it.
- Store candidate checkpoint values in a pending run manifest alongside the
  NDJSON and report. After manual editing and successful publisher staging,
  the operator explicitly acknowledges that run. The acknowledgement records
  the final edited file hash and confirms that the publisher's accepted count
  equals the number of non-empty NDJSON records before committing eligible
  source checkpoints.
- Only sources whose data was successfully processed may advance. Disabled,
  failed or unavailable sources retain their previous checkpoints. A healthy
  source may advance even when another source failed, provided the accepted
  staging included the reviewed merged output derived from that run.
- If publisher staging fails, the accepted count is unexpected or the edited
  file cannot be tied to the pending run, do not advance any candidate
  checkpoints for that publication. Inspect publisher status before retrying;
  do not blindly repeat mutating POSTs. Category-inventory-only runs never
  advance checkpoints.
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
- Support dry-run/preview, one-shot export and repeatable polling. Keep grabbing,
  manual review and publishing as separate operator actions. Schedule source
  collection via systemd timer only after the manual-approval workflow is defined;
  run network source access through the configured SOCKS5 or WireGuard/netns tunnel.
- Persist checkpoints, deduplication state and retry scheduling.
- Bound timeouts, result count, download size and concurrency; respect rate limits.
- Log source status without keys, cookies, credential-bearing URLs or private data.
- Report fetched, parsed, rejected, duplicate, updated, media-uploaded and published
  counts.

## Tests and acceptance

- Small sanitized fixtures for the CSV header/rows, RuTracker XML/XZ (nested and
  single-file layouts, forum filter and deletion), raw-DEFLATE Rutor JSON, Torznab
  capabilities/recent RSS items and JSON/HTML/text templates. Never commit production
  dumps, full CSV snapshots or large source caches.
- XZ tests cover valid, truncated and corrupt streams, compressed/expanded byte
  limits, decoder memory limits and DTD/entity rejection.
- RuTracker deletion must use an already published shard or be quarantined; a
  later higher-priority live observation can restore a tombstoned infohash.
- Torznab tests use local mock endpoints, include a failed indexer alongside a
  healthy one, verify capability/category parsing, API-key redaction, redirect
  refusal, verified hash extraction and idempotent overlap replay.
- Template tests cover JSON paths, HTML selectors/attributes, named text groups,
  malformed fields, regex bounds and no dynamic code execution.
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

## Unified category summaries and visible sources

All adapters now expose `category_group_summary` in the source JSON and per-source
category JSON, using the same HTML Category / Included forums / Active / Deleted /
With size / Without size / Payload sum / Catalog NDJSON / Catalog zlib estimate
columns. RuTracker groups use `category_groups` and provenance forum IDs; other
adapters aggregate stored native/heuristic categories. `Included forums` is — when
not applicable. Other (unlabelled records) is last. Native categories remain in
collapsed details. Multi-label sources may overlap across categories; sizes and
counts of category rows are not necessarily additive. Source totals deduplicate.

HTML now completely omits disabled sources, even with cached data, and sources
with no stored observations. This applies to overview, details, pairwise tables,
coverage matrix, source count and visible totals. Enabled unselected cached
sources with data remain visible; failed sources with retained data retain a
warning. JSON keeps all diagnostic statuses/raw cached metrics and legacy totals;
`visible_totals` and `source_coverage_matrix` correspond to the HTML selection.
A failed source with no cached rows is hidden in HTML; consult JSON diagnostics.
No cached SQLite records are deleted by visibility settings.

### Anime in all three snapshot sources

- Rutor: existing native Anime category is retained, never merged into Movie/Series.
- RuTracker: Anime uses explicit forum IDs from the media profile's category_groups;
  copy both category_groups and forum_include_ids into a private profile. This
  does not guess anime from titles in unrelated forums.
- CSV: the example profile now checks Anime **before** Video and Music in `first`
  mode. Conservative patterns match explicit anime/аниме words and known bracketed
  fansub release labels (SubsPlease, HorribleSubs, Erai-raws). Matching patterns
  are also added to title-filter include rules so such rows can enter inventory.
  These are configurable heuristics, not complete detection; generic Japanese
  names, resolutions and codecs alone are not evidence of Anime. Unknown anime
  may remain Video or be excluded. No public dataset is fetched for tests.

Existing private profiles are not rewritten automatically. Copy the Anime group
and its include expressions from `tools/grabber-profile.example.json`, place it
first, retain `inventory_category_mode: "first"`. Policy fingerprint changes
trigger a CSV rescan. This change is for inventory classification only, not a
new publishing classifier. Compression remains an estimate, not final client DB.

## Report-only, progress and base versus extra estimates

To update tables using an existing inventory database **without opening/hash-reading
CSV/Rutor/XML or accessing feeds**, use:

```sh
python3 -I tools/grabber.py inventory --report-only \
  --profile /path/to/profile.json \
  --state /path/to/inventory.sqlite3 \
  --report /path/to/inventory-report.json
```

Keep sources enabled if they should appear in HTML. `--source` still selects
sources for status/refresh reporting; the report can compare cached others.
`--report-only` requires an initialized existing inventory DB, keeps observations
and checkpoints unchanged, marks selected sources cached, and recomputes tables,
group rules, compression and intersections from stored data. It does not reapply
input title/forum filters, extract missing fields, detect updated input files or
recover discarded descriptions. JSON records `report_only` and warns of cached scope.
No publishing/ACK is performed. Back up the DB as usual; do not run refresh and
report-only concurrently. This avoids a new hour-long XML import, **not** all CPU:
several SQLite passes/compression/intersections on millions of rows still take time.

Inventory CLI now reports progress to stderr (stdout remains final JSON): source,
stage, elapsed, approximate overall percent and stage ETA. Overall ETA stays
unknown until remaining-stage durations can be estimated; it is 0 only at completion.
`--no-progress` disables it. A heartbeat continues during blocking SQL/hash/parse.
Overall weights are heuristic (input 0–70%, report 70–93%, SQL 93–98%, write 98–100%;
report-only assigns 0–93% to cached reporting). CSV uses bytes read (buffer read-ahead),
Rutor uses known item count, XML uses compressed-byte position, report passes use
SQLite row counts. XZ read-ahead/dictionary and uneven records make byte ETA rough;
Overall percentage weights represent work allocation, **not elapsed-time fractions**;
they are never extrapolated into a global ETA. Fast earlier sources do not predict
the cost of a later XML scan. Unknown-total feeds/SQL stages show unknown ETA, not a fabricated exact
completion time. 100% is emitted only after successful report writing. This progress
slice covers `inventory`; publisher `run`/ACK are not changed into a scheduling UI.

Base fields are exactly the canonical fields currently produced by the Rutor
adapter: `ih`, `title`, `date`, `size`, `tags`, `trackers`. `files`, media refs,
source-specific fields and all retained provenance (including Rutor Name/Names,
quality/IDs/seed metrics) are **extra information**. This is a diagnostic sizing
policy, not a wire-format change or deletion of useful metadata.

Overview, source cards and category summary rows compare four sizes:
`base_ndjson_bytes`, `base_zlib_estimate_bytes`, `with_extra_ndjson_bytes`,
`with_extra_zlib_estimate_bytes`. The full estimate serializes other record fields
and retained provenance under `extra_info`, excludes source file paths/revision/run
wrappers, and includes newlines. Legacy `catalog_*` metrics still describe original
normalized records for compatibility. All sizes exclude tombstones; payload-size
columns are sizes of shared media files, not catalog size. Compression uses Python
zlib level 6 with <=8 MiB segments, not exact portable publisher codec bytes or
final client SQLite/index/manifest/media-pack size. Enabled extraction options only
affect future scans: report-only measures what is actually present in SQLite.

Other is omitted from HTML when **all numeric fields**, including forum count and
size estimates, are zero/null. A deletion-only Other or an Other with observed
forums is not hidden. JSON retains the row for deterministic machine processing.
