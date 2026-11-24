# Stage 3 media packs, shards and snapshots

## Implemented storage contract

`Mesh` now publishes and verifies real description/poster packs, record shards, local
selection policies and compacted snapshots. This extends signed manifest version `v=1`
without changing the portable built-in zlib record codec. No external zlib/zstd dependency
was added. Old manifests without `packs`, segment byte counts or shard names remain readable.

A list is still its 32-byte ed25519 public key, represented as 64 lowercase hex characters.
The **canonical bencoding of the entire manifest dictionary except `sig`** is signed.
Consequently file descriptors, pack kinds, sizes, hashes, shard names, sequence numbers and
snapshot ancestry are authenticated. An object reference is authenticated through its record
segment's signed SHA-256 descriptor; there is no unsigned sidecar supplying media locations.

### Published files

```text
<data>/lists/<pubkey>/
  manifest.ben
  records/seg-NNNNNN.z       # append history before the first snapshot; legacy compatible
  records/s-<64 hex>.z       # immutable version-scoped identity after a snapshot
  media/p-<64 hex>.bin       # immutable raw-byte packs, random collision-resistant identity
  pending.ndjson            # existing local publisher queue; excluded by the torrent builder
```

New segment descriptors in `segs` contain:

* `f`: validated basename, never a relative traversal or absolute path;
* `h`: **32 raw bytes** of SHA-256 over the compressed segment;
* `b`: signed compressed byte length;
* `n`: signed nonblank NDJSON record count, checked while decoding;
* `s`: shard name, checked against every record in that segment.

New `packs` descriptors contain `f`, `h`, `b` and `k`, where `k` is `description` or `poster`.
A pack holds only one kind, can hold multiple objects, and is at most 16 MiB. Uploads append
only to unpublished staging files. Once released, that pack is never appended or rewritten;
a subsequent upload gets a new pack. Snapshotting writes fresh packs, then switches signed
references in one manifest commit.

Records retain `ih`, `title`, `size`, `files`, `date`, `tags`, `trackers` and legacy `poster`
URL fields. `shard` defaults to `default`; explicit names are 1–64 lowercase ASCII letters,
digits, underscores or hyphens. Tombstones are `{"del":"<ih>","shard":"movies"}`. A publisher
must put a tombstone in the old shard when moving an infohash to another shard; consumers
may subscribe to only the old shard. There is no cross-shard implicit deletion.

Media references use these exact JSON fields:

```json
{
  "ih": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "title": "Example",
  "shard": "movies",
  "description_ref": {
    "pack": "p-<64 lowercase hex>.bin",
    "offset": 0,
    "length": 123,
    "sha256": "<64 lowercase hex over exactly the object bytes>",
    "type": "text/plain; charset=utf-8"
  }
}
```

`poster_ref` uses the same structure. Description objects are valid UTF-8, NUL-free, at
most 64 KiB, with exactly `text/plain; charset=utf-8`. Poster objects are at most 2 MiB;
only `image/jpeg`, `image/png` and `image/webp` with the matching format magic are served.
SVG/HTML and arbitrary MIME types are refused. Images are **not decoded or dimension-checked**
by the daemon. Legacy poster URLs are returned as metadata only, never fetched server-side.

Every media reference must fit inside a signed pack of the correct kind. Negative lengths,
overflows, unknown packs and invalid hash strings are refused. Publishing also verifies the
actual slice bytes and MIME before committing. Media reads check the full pack SHA-256 and
the object SHA-256 before serving or caching. Descriptions/posters are not inflated into the
search index, nor eagerly read by catalog search, list loading or manifest verification.

### Local state (never torrent payload)

```text
<data>/mesh-local/<pubkey>/
  policy.json
  staging/p-<id>.bin
  staging/p-<id>.bin.kind
  cache/<object sha256 hex>
  original.torrent          # original info dictionary, never rebuilt from a partial directory
  original.pending.torrent  # crash-recovery sidecar during version acceptance
```

Local policy has `shards`, `record_bytes`, `media_bytes`, `cache_bytes` and `mirror_media`.
`mirror_media` defaults to false; true schedules complete media packs within the media budget.
Defaults for the other fields are
`["*"]`, 256 MiB of compressed records, 256 MiB of retained packs and 32 MiB of cached
objects **per list**. Empty `shards` means none, not all. Budgets are nonnegative and bounded
by 16 GiB each; zero is meaningful. Local state is outside `lists/`, so the existing torrent
builder cannot accidentally publish policy, cache objects or unpublished media.

Admission is **whole-shard**, not a prefix of a shard's append log: sum all compressed files
of requested shards, then admit complete shards in lexicographic shard-name order while
the record budget allows. `plan()` exposes `selected=false` for non-admitted files. If even
one selected file of a shard is absent, **none of that shard is indexed**. This prevents a
missing later update/tombstone from exposing stale or deleted records. Missing signed files
are legal for metadata-first imports; corrupt selected files are not legal. Installing the
last missing selected file rebuilds that shard's current state.

Changing a subscriber policy prunes unselected record files and over-budget/unreferenced
media files, rebuilds the index, and persists across restart. Owners cannot evict their own
published shards/packs through policy. Media cache objects are verified again on every hit;
a poisoned entry is removed, then repaired from a verified pack if available. Eviction uses
oldest cache-file modification time (insertion order, not strict access-time LRU). The cache
is per-list and content-addressed, not a cross-list deduplication service.

Disk budgets cover retained files, not filesystem allocation granularity, torrent piece
buffers, `.tmp` files, the isolated network working copy or temporary transaction overlap.
The network working copy may duplicate retained payloads; old/new generations can overlap
until asynchronous torrent retirement finishes. Selected record admission and reserved media
pack bytes bound each active version's requested payload, not the combined disk footprint. Unpublished staging has a separate
ceiling equal to the list's `media_bytes`. A publish/snapshot can temporarily hold old,
new and staged payloads until commit. An API/file failure rolls newly created payloads back;
restart prunes orphan committed-directory files. Files are bounded **before** allocation;
media disk reads are serialized by the writer mutex (one <=16 MiB pack plus object buffers
at once). There are at most 4096 segment descriptors and 4096 pack descriptors, a 4 MiB
manifest, 64 KiB record lines, one million decoded records, and 256 MiB aggregate decoded
record text in a selected version.

## Large-catalog resource profile

Linux CI runs `tools/mesh-scale-test.py` against two real `p2playd` processes. The
publisher creates 20,000 records in the selected `movies` shard and 512 records in an
unselected `series` shard. Near-limit titles make the normalized records cross the
publisher's 8 MiB chunking boundary; one representative movie references a shared
description at the 64 KiB object limit and a shared JPEG poster at the 2 MiB limit.
The test also verifies that one-byte-over-limit media objects are rejected.

The subscriber uses explicit local peer hints with DHT disabled and a bounded test policy:
64 MiB of selected compressed records, 8 MiB of retained media and 8 MiB of cache. The
acceptance checks prove that:

* all selected movie records are indexed, while the series records and their segment
  files remain absent;
* the initial subscription does not mirror description/poster packs;
* a poster and description request fetches and verifies the corresponding whole packs,
  populates content-addressed object cache entries and survives a cold restart;
* the selected record payload fits its configured disk budget.

The run reports publication and subscription time, segment count and compressed bytes,
publisher/subscriber file bytes, torrent working-copy bytes, cache bytes, search latency,
cold-start time and process RSS. Linux loopback RX/TX deltas are captured separately for
the initial subscription and lazy media requests. Those counters include BitTorrent
protocol and local HTTP test traffic; they are not a pure payload counter.

No hard wall-clock or RSS pass/fail threshold is imposed by this first profile because
the GitHub runner class and concurrent load can vary. The first green CI measurements
establish a repeatable baseline before introducing platform-sensitive performance
budgets. Protocol and per-list storage limits remain hard assertions in the test.

### First observed baseline (Linux CI run #103)

The first green run, associated with master commit `8e4c8d68b35eac1588d8a2a8263cef2c5ef11f0d`,
completed the full profile. These are observations from one GitHub-hosted runner, not
performance guarantees or admission thresholds:

| Measurement | Run #103 |
| --- | ---: |
| Publish time | 1.141 s |
| Subscribe time | 6.776 s |
| Search time | 0.0244 s |
| Cold daemon start | 0.402 s |
| Uploaded record text | 9,649,544 B |
| Selected movie record segments | 2 / 413,283 B compressed |
| Selected record files on subscriber | 414,270 B |
| Publisher list directory, final | 2,585,059 B |
| Subscriber data, final | 2,576,958 B |
| Torrent working copy, before / after media | 414,049 B / 2,576,737 B |
| Media cache after the two requests | 2,162,688 B |
| Loopback RX/TX, initial subscription | 562,287 B / 562,287 B |
| Loopback RX/TX, lazy media requests | 4,509,236 B / 4,509,236 B |
| Subscriber RSS, before / after cold restart | 116,215,808 B / 76,402,688 B |

The fixture contains 20,000 selected `movies` records and 512 unselected `series`
records. The second shard is not downloaded or indexed. The reported media-cache size
corresponds to the verified 2 MiB poster and 64 KiB description, stored in their two
signed packs. RSS high-water marks were 124,342,272 B before restart and 134,705,152 B
after restart; these runner-specific values are retained as context, not limits.


## HTTP API

The existing native API authentication applies. Publishing, selection changes, local file
installation, snapshots and removals require administrator rank. Reads require the normal
catalog authorization. These routes do not bypass authorization or fetch arbitrary URLs.

| Method and route | Request / result |
| --- | --- |
| `POST /api/publish/:pk/media/description` | Raw body, `Content-Type: text/plain; charset=utf-8`; returns a reference with status 201. |
| `POST /api/publish/:pk/media/poster` | Raw JPEG/PNG/WebP body and exact allowed Content-Type; returns a reference. |
| `POST /api/publish/:pk/records` | Existing NDJSON API; use returned references in `description_ref` / `poster_ref` and optional `shard`. |
| `POST /api/publish/:pk/release` | Existing sequence-incrementing release, now also signing packs. |
| `POST /api/publish/:pk/snapshot` | Owner-only compaction; fails if pending records have not been released. |
| `GET /api/catalog/:ih/description?list=:pk` | Verified text bytes. |
| `GET /api/catalog/:ih/poster?list=:pk` | Verified image bytes. |
| `HEAD` on either media route | Same validated size/type headers, no response body. |
| `GET /api/lists/:pk/files` or `/policy` | `{ "policy": {...}, "files": [{path,sha256,bytes,records,shard,kind,selected,present}] }`. |
| `POST /api/lists/:pk/policy` | Partial JSON update, e.g. `{"shards":["movies"],"media_bytes":16777216,"cache_bytes":1048576}`. |
| `POST /api/lists/:pk/files` | Admin local install: `{"path":"media/p-...bin","source":"/downloaded/file"}`; verifies signed size/hash and budget. |

Media routes default to trying record-holding lists in key order. Specify `list` when a
particular source/version is required. A missing record/reference is 404. An unavailable
pack or integrity failure is 503, not an empty success. For a missing or corrupt pack on an
active network holder, the GET/HEAD miss queues a bounded asynchronous pack request; clients
receive `Retry-After: 2` and must retry. It never fetches legacy URLs or blocks HTTP until a
download finishes. A signed object/hash/type mismatch remains an explicit 503.
Responses include `X-Content-Type-Options: nosniff` and private cache-control. Search and
record JSON expose references, not inline media bytes.

A snapshot replays live records, removes tombstones and duplicate historical updates,
repackages only live media slices (deduplicating identical kind/hash/type objects), and signs
new record and pack descriptors at `seq+1`. `snapshot` records the previous sequence. Only
after the new signed manifest has been atomically renamed into place are old segments/packs
removed. A failed release leaves the old manifest and pending records intact. Rename-based
commits provide process-level atomicity, **not power-loss durability**: there is no fsync
journal and a crash between manifest commit and pending-file removal may replay pending
records in a subsequent release. Replaying those records is state-idempotent but increments
sequence. Seed keys remain owner-only local files.

## Implemented selective network integration

`Core` integrates the signed storage plan with libtorrent in `mesh_net.cpp`; this is no
longer a storage-only change. BEP 44 DHT pointers and signed BEP 10 peer pointers share the
same joined worker verification/download path (see [peer-items.md](peer-items.md)).

1. A pointer pins the original **v2 infohash, list key and sequence**. Initial torrent
   metadata uses `default_dont_download`; only `manifest.ben` is then prioritized. The
   signed manifest is accepted with the expected key/sequence even while payload files are
   absent. File roots, symlinks, counts and manifest size are checked. Before enabling any
   record/media payload, torrent file paths and sizes must match the signed descriptor
   allowlist; unsigned extras and absent signed files fail closed.
2. `Mesh::plan()` maps to torrent file indices after stripping `<pk>/`. Only whole admitted
   record shards receive high priority. Media files stay `dont_download` unless already
   retained, requested by an HTTP miss or admitted by `mirror_media`. Padding stays at zero
   priority. Metadata and manifest acceptance do not require a full torrent finish.
3. Per-file piece-granularity completion calls `Mesh::installFile`, which verifies signed
   size/hash, enforces retained budgets and rebuilds only complete shard histories. The
   state API exposes `records_ready`, `media_pending`, `error`, `version_ih` and
   `selective: true`. `seeding: true` means a current holder handle exists; it does **not**
   mean all files are present or that the node is a complete seed. `plan().selected` is
   shard admission, not the transient network priority of a media pack.
4. GET/HEAD media misses deduplicate by list/version/pack. The queue plus active HTTP media
   requests are capped at 64 across lists; active requests expire after 60 seconds. Media
   packs reserve the per-list media budget before download, including partially downloaded
   packs. Mirroring is also pack-budget limited. Successful reads populate and verify the
   content-addressed object cache. Queue rejection or an unavailable peer still yields 503;
   clients must retry rather than assume a guaranteed fetch.
5. `mesh-local/<pk>/original.torrent` retains the original info dictionary and signed
   pointer, excluding optional v2 piece layers (peers supply authenticated hash trees).
   The pending sidecar is written before manifest commit and permits restart recovery.
   Partial subscribers never reconstruct a different torrent from retained files. Imports
   without authenticated original metadata remain offline holdings rather than inventing
   a network identity. Publishers can rebuild their complete version torrent.
6. Libtorrent reads/writes only isolated generation directories under
   `lists/.network/<pk>-<generation>/`, not mutable retained list directories. The joined
   worker and API version/policy/remove mutations share a lifecycle lock; network callbacks
   only use bounded queues/snapshots. Snapshot/policy pruning cannot delete a live torrent's
   files. Removal is asynchronous: old working copies are queued for cleanup only after a
   delete job's success **or failure** alert. This distinction matters for partial torrents
   and extra empty directories; a failed delete must not permanently gate the next version.
   Startup cleans abandoned working generations before creating mesh handles. No partial
   holder uses `seed_mode`; even copied complete files are checked by libtorrent.
7. Owner releases expose the newly signed pointer before returning even when the old
   handle is still retiring; the worker completes reseeding afterward. Received pointers
   retain rollback/equivocation protection and their original signatures. Subscription
   peer hints bootstrap both directory and version torrents, including with DHT disabled.

### Explicit remaining gaps

* Lazy media transfer is **whole-pack**, not an exact object slice or HTTP Range request.
  Requesting one object can download unrelated objects from its containing pack. There is
  no per-object/range-based piece scheduler.
* Directory `have` announces list versions/holders, not a signed shard/media availability
  inventory. Ordinary BitTorrent piece availability describes the partial data actually held.
* Quotas are per-list, not a global multi-list disk quota; no UI controls are added.
* Owner key rotation/recovery and signed cross-shard migration semantics are not added.
* Local imports/install sources are admin-selected paths; the native filesystem sandbox
  and external downloader's staging safety remain their caller's responsibility.
* No binary image decoding/pixel limits, HTTP range-serving of media objects, power-loss
  fsync journal, or cross-process/multi-daemon lock on a shared data directory.
* No platform SDK runtime tests here; production code uses existing filesystem, crypto
  and built-in zlib facilities. Tests run on the native Linux build.

## Tests

```sh
python3 -I tools/mesh-media-test.py --build-dir build-linux build-linux/src/app/p2playd
python3 -I tools/mesh-media-test.py --build-dir build-linux --sanitize
python3 -I tools/mesh-test.py build-linux/src/app/p2playd
python3 -I tools/peer-items-test.py build-linux/src/app/p2playd
python3 -I tools/mesh-selective-test.py build-linux/src/app/p2playd
python3 tools/meshnet-test.py build-linux/src/app/p2playd  # requires Python libtorrent binding
```

The new Python runner needs only the standard library. The optional C++ harness uses the
actual compiled project's libtorrent crypto and production Mesh/Torrs sources; no mock
cryptography or replacement pack implementation. `--build-dir` supports native CMake Unix
Makefiles builds. Without a build directory, system `pkg-config libtorrent-rasterbar` can
supply compiler/linker flags. Passing only a daemon path runs the API checks without a C++
compile or Python libtorrent dependency.

Coverage includes append offsets/immutable packs, safe media types and UTF-8, signatures,
malicious descriptors and offsets, SHA-256 pack/slice/cache verification, cache poisoning,
whole-shard selection and quotas, absent-history tombstone safety, descriptors-only imports,
verified lazy file installation, restarts, snapshot repacking, failed-publish rollback,
concurrent HTTP cache requests, authorization, binary/HEAD responses and removal cleanup.

The selective network test uses two real daemons and v2 transfers without Python bindings.
It checks manifest-first admission, unselected files absent from network storage, lazy
HTTP-triggered packs, mirroring/budget changes, original-infohash partial restart,
policy reselection, live snapshot generation changes and removal with queued media work.
The storage harness also checks pointer/manifest key and sequence mismatches before commit.


## Incremental version refresh

A regular publisher release keeps the old immutable record descriptors in the
signed manifest and adds new segment files for the incoming rows. The version
torrent therefore still describes the complete selected version, but its old
record paths and hashes are unchanged.

When a subscriber accepts a newer pointer, `meshFetch` seeds the new torrent
working directory with selected record files already held for the previous
version. It copies only files that were present and whose byte length and
SHA-256 match the previous signed descriptor. Libtorrent then checks the copied
bytes against the new v2 torrent; the new manifest and every descriptor are
still verified by the normal acceptance path. New or unavailable segments are
downloaded normally. This is a local reuse optimization, not a wire-format
change and not trust in an unsigned cache.

The live `meshnet-test.py` scenario now publishes version 1, subscribes a
second node, then publishes version 2 with both a new infohash and a complete
replacement row for an existing infohash. It checks that the second node
preloads the previously held immutable segment and observes the replacement
row. This is a correctness check for the reuse path; it is not yet a benchmark
of exact network bytes for repeated large-catalog updates.

Snapshots reset append history and write fresh compacted segments. Subscribers
may need to fetch selected record shards again, so compaction should be
threshold-driven rather than coupled to every grabber/RSS poll.

The owner-only `POST /api/publish/:pubkey/replace` operation is distinct from
compaction. It accepts a complete live-record NDJSON file, constructs a fresh
snapshot containing exactly those records, preserves the list public key and
switches the signed manifest once. Omitted infohashes disappear; empty input
clears the catalog. Old append history and unreferenced media packs are pruned
after commit. Subscribers may need to fetch selected record shards again.
