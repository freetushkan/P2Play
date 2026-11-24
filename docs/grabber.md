# Catalog grabber — design notes

Status: requirements for the future catalog-ingestion tool. The exact input
formats and sample files will be supplied later; do not invent them now.

## Goals and placement

- Implement the grabber in **Python** as a separate operator/developer tool,
  not as part of the C++ daemon or a client runtime dependency.
- Use a clean pipeline: source adapter → parsed item → normalized catalog
  record → validation/deduplication → NDJSON / MediaMesh publishing.
- Adding a format should mean adding an adapter, not rewriting normalization
  or publisher code.
- Support one-shot imports and scheduled incremental refresh of recent releases.
- Keep credentials, API keys, cookies and personal data out of Git.

## Input formats

The DOCX names these adapter families; sample fixtures are pending:
RuTracker XML/HTML dumps, Rutor JSON dumps, dirty line-oriented text dumps
(non-media entries may be tagged Other), RSS feeds, tracker pages and APIs.
Adapters keep an ID/date checkpoint and fetch only new/changed entries. Normalize
hashes to lowercase hex, title, size, file count, dates and categories mapped to
publisher tags. Deduplicate by infohash and resolve conflicts through configured
source priorities. Malformed input and invalid hashes must be reported.

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

- Unit fixtures for each input format after examples are supplied.
- Torznab fixtures for caps, search and recent items; direct RSS fixtures if used.
- Cover malformed XML, missing GUID/date/hash, invalid magnet, duplicate GUID/hash,
  changed metadata, pagination, retries and overlapping poll windows.
- Replaying a poll must be idempotent; dry-run must not touch publisher state.
- Integrate with a local/mock Torznab endpoint, not public trackers or private keys.
- End-to-end: publish a small signed release and verify normalized records and
  signed description/poster refs from a second node.
