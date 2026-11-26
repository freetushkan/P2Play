# MediaMesh v1 — wire protocol and list format

**Status:** implementation profile for the current P2Play/MediaMesh v1 code. This is a
public-facing draft, not a claim that an independent implementation has passed
interoperability tests. The normative words **MUST**, **MUST NOT**, **SHOULD** and **MAY**
describe the behavior required to interoperate with the current implementation.

The protocol is deliberately split into two layers:

1. the directory swarm and BEP 10 extension exchange signed pointers;
2. ordinary BitTorrent v2 transfers the version torrent identified by a signed pointer.

DHT is an additional discovery/publication route. It is not a trust root and is not
required when a directory peer is reachable.

## 1. Identifiers and byte conventions

A list is identified by its 32-byte Ed25519 public key. Its textual API representation is
64 lowercase hexadecimal characters. Names and descriptions are metadata, not identifiers.

A list sequence (`seq`) is a nonnegative signed 64-bit integer. A publisher increments it
for every release or snapshot. Consumers MUST NOT accept a lower sequence after a higher
one has been accepted. Two different signed pointer values for the same key and sequence
are an equivocation and MUST NOT replace one another.

Unless a field is explicitly described as hexadecimal or UTF-8 text, protocol strings
are byte strings. Public keys, signatures, hashes and infohashes are raw bytes on the wire,
not their hexadecimal representations. Integers use canonical bencoding.

## 2. Directory swarm

All participating nodes join the same fixed BitTorrent v1 swarm. Its single file is named
`mediamesh.v1`, has a piece length of 16,384 bytes and contains these exact 59 bytes:

```text
MediaMesh v1 directory: the nodes holding lists meet here.
```

The corresponding v1 infohash is:

```text
18afe4a5bf3024b5ff70d391bfed0f11bc97c9c9
```

The directory file is only a rendezvous identity. Implementations MUST NOT assume that a
peer has downloaded or is seeding its payload. The MediaMesh extension is enabled only on
this directory swarm, not on arbitrary content torrents.

## 3. BEP 10 negotiation and framing

The extension name is `mediamesh`. Each peer advertises its local receive message ID in
the BEP 10 extension handshake dictionary `m`, for example:

```text
{"m": {"mediamesh": 23}, "p": 6881}
```

`23` is the current P2Play receive ID, not a globally fixed ID. A sender MUST use the
message ID advertised by the remote peer. The optional `p` field is the peer's listening
port and may be used as a connection hint; it is not an authenticated identity.

Messages use the BitTorrent extended message ID `20`, followed by the negotiated
`mediamesh` message ID, followed by one bencoded dictionary. The current implementation
accepts at most 512 KiB of bencoded message body, with bdecode depth 8 and 17,000 tokens.
A message exceeding the outer size limit causes the connection to be disconnected.
Unknown, malformed or trailing-data messages are ignored.

The dictionary MUST contain exactly one of the message forms below. Keys shown as
`bytes32`, `bytes64` or `bytes` are bencoded byte strings.

### 3.1 `have`

```text
{"have": [[bytes32 list_key, integer seq], ...]}
```

`have` is a complete snapshot of the sender's currently held lists, not a delta. It MAY
contain up to 4,096 entries. Each key MUST be exactly 32 bytes, each sequence MUST be a
nonnegative signed 64-bit integer, and keys MUST NOT be duplicated.

A receiver replaces the previous snapshot for that connection when it accepts a new
`have`. The information is untrusted discovery metadata: it MUST NOT advance the signed
sequence floor, authorize a download by itself, or be treated as proof that a version is
valid.

The current node sends an initial snapshot after negotiation and refreshes it periodically.
A sender SHOULD send an updated snapshot before pushing an item for a newly published
sequence.

### 3.2 `known` inventory gossip

```text
{"known": [[bytes32 list_key, integer seq], ...]}
```

`known` is a complete snapshot of keys the sender knows about: its locally held lists
plus keys learned from `have` or other `known` snapshots. It is bounded to 4,096 unique
entries. A node sends the snapshot after extension negotiation and refreshes it when the
inventory changes; newly learned entries are therefore relayed to other connected directory
peers, allowing multi-hop gossip through the connected peer graph.

The sequence is an untrusted hint, just like the sequence in `have`. `known` MUST NOT be
used as evidence of a local copy, a subscription, or popularity; it MUST NOT increment
`holders`, affect list ranking, authorize an `item` push, or advance the signed sequence
floor. Receivers may use it to request a signed pointer through the normal bounded `get`
path. Older peers ignore this unknown message form.

A verified pointer may be cached for the Discover card without subscribing. This discovery-
only state MUST NOT start a version torrent. Version transfer still requires an explicit
subscription or an already-held list that needs a newer signed version.

### 3.3 `get`

```text
{"get": bytes32 list_key, "seq": integer known_seq}
```

`known_seq` MUST be at least `-1` and fit a signed 64-bit integer. The requester asks for a
signed pointer for `list_key` whose sequence is strictly greater than `known_seq`. The
request dictionary MUST contain exactly these two keys and its encoded body MUST be no
larger than 128 bytes.

If the peer has no pointer, or its pointer sequence is less than or equal to the supplied
sequence, it sends no reply. Otherwise it replies with an `item` message. Requests are
bounded per connection; the current implementation allows 32 outstanding keys, retries
after 15 plugin ticks and rotates through the request snapshot.

### 3.4 `item`

```text
{"item": {
  "k": bytes32 list_key,
  "salt": "mediamesh.v1",
  "seq": integer,
  "sig": bytes64 signature,
  "v": {
    "ih": bytes32 version_v2_infohash,
    "n": bytes list_name
  }
}}
```

The outer message contains only `item`. The inner envelope contains exactly `k`, `salt`,
`seq`, `sig` and `v`; `v` contains exactly `ih` and `n`.

Requirements:

- `k` MUST be the 32-byte list public key.
- `salt` MUST be the exact byte string `mediamesh.v1`.
- `seq` MUST be a nonnegative signed 64-bit integer.
- `sig` MUST be exactly 64 bytes.
- `v.ih` MUST be a nonzero 32-byte BitTorrent v2 infohash.
- `v.n` MUST be present and no longer than 64 bytes.
- The bencoded `v` value MUST be no larger than 1,000 bytes.
- The complete encoded inner envelope MUST be no larger than 2,048 bytes.
- Decoding is limited to depth 8 / 64 tokens. Duplicate keys, noncanonical encodings and
  trailing bytes MUST be rejected.

An `item` is accepted by a receiver only in one of two cases:

1. it answers an outstanding `get` for the same key on that connection; or
2. it is a push whose key and sequence exactly match an entry in the sender's latest
   accepted `have` snapshot.

A receiver MUST ignore unsolicited items that meet neither condition. After admission to
the bounded worker queue, it MUST verify the BEP 44 signature and local subscription/holding
policy before starting a version download.

### 3.4 Budgets

The current implementation enforces these per-connection, per-plugin-tick budgets:

| Operation | Limit |
| --- | ---: |
| Served `get` requests | 32 |
| Sent push `item` messages | 32 |
| Received `item` messages | 32 |
| Received `have` updates | 2 |
| Outstanding `get` keys | 32 |

A sender's `have` and request snapshots are each bounded to 4,096 keys. These are resource
limits, not protocol negotiation parameters. Implementations SHOULD make limits explicit
and bounded; changing them requires an interoperability/security review.

### 3.5 Independent wire-peer test (non-normative)

The repository includes `tools/mediamesh_reference_peer.py`, a small reference
implementation of BitTorrent handshake/framing, BEP 10 extension negotiation and
bencoding using only the Python standard library. It does not import or link P2Play
code. Fixed canonical payload fixtures live in
`tools/mediamesh-v1-wire-fixtures.json`.

Run the live interoperability test with:

```sh
python3 -I tools/mediamesh-interop-test.py build-linux/src/app/p2playd
```

The test checks the reference peer's parsing of `have`, its `get` request and the
returned `item`; then it subscribes a second daemon and has the reference peer answer
that daemon's `get` with the publisher's signed envelope. The receiver must fetch the
version torrent through the advertised peer hint and relay the identical envelope.

This is a BEP 10/wire-format interoperability test, not a complete independent
MediaMesh implementation. The reference peer treats the 64-byte BEP 44 signature as
opaque; cryptographic signature verification, manifest verification and content-hash
validation remain the receiving daemon's responsibility and are covered by the
daemon's security/integration tests. The test does not specify or test a search RPC,
version-torrent wire implementation, DHT implementation, or pPlay UI.

## 4. Signed pointer and BEP 44

The pointer is a BEP 44 mutable item under the list public key, with salt `mediamesh.v1`.
Its value is the canonical bencoding of:

```text
{"ih": bytes32 version_v2_infohash, "n": bytes list_name}
```

The BEP 44 sequence is the list sequence. The 64-byte Ed25519 signature MUST be verified
using the BEP 44 mutable-item signing rules, including the exact salt and sequence. The
BEP 10 `item` wrapper is not part of the signed value.

The BEP 10 inner envelope is the same signed object relayed unchanged. A relay does not need
the list's private key. A valid `have` entry is never a substitute for this signature.

The current DHT and BEP 10 paths feed the same pointer validation/acceptance logic. DHT may
be disabled without disabling peer-item exchange.

## 5. Version torrent

The signed pointer's `v.ih` identifies a BitTorrent v2-only torrent with a piece length of
65,536 bytes. The torrent's top-level directory is the lowercase hexadecimal list key.
It contains `manifest.ben` and only files named by the signed manifest:

```text
<list-key>/
  manifest.ben
  records/<segment filename>
  media/<pack filename>
```

The version torrent MUST have the v2 infohash named in the pointer. Consumers MUST also
check that the downloaded manifest's public key and sequence exactly match the pointer.
A mismatch is a failed import, even if the manifest has a valid signature of its own.

## 6. Manifest v1

`manifest.ben` is a bencoded dictionary. Required fields are:

| Key | Type | Meaning |
| --- | --- | --- |
| `v` | integer | Schema version, exactly `1` |
| `pk` | bytes32 | List public key |
| `seq` | integer | Nonnegative signed 64-bit sequence |
| `name` | bytes | Nonempty list name, at most 256 bytes |
| `desc` | bytes | Description, at most 4,096 bytes |
| `segs` | list | Record segment descriptors, at most 4,096 |
| `packs` | list | Optional media pack descriptors, at most 4,096 |
| `meta` | list | Optional signed meta-list entries; presence marks this list as a meta-list |
| `sig` | bytes64 | Ed25519 signature |

The signature covers canonical bencoding of the entire manifest dictionary with `sig`
removed. Implementations MUST verify the signature against `pk` before trusting any file
descriptor, count, name or metadata.

The optional `meta` field is a list of at most 256 entries. Its presence (including an empty
list) identifies a meta-list; ordinary catalog lists omit the field. Each entry is a bencoded
dictionary containing `pk` (32-byte public key of a child catalog), `note` (UTF-8 annotation,
at most 2,048 bytes) and `enabled` (integer 0 or 1). Child keys MUST be unique and MUST NOT
equal the parent key. The complete array and every enable flag / annotation are covered by
the parent's Ed25519 signature.

New record segment descriptors contain:

| Key | Type | Meaning |
| --- | --- | --- |
| `f` | bytes | Safe segment filename |
| `h` | bytes32 | SHA-256 of the compressed segment |
| `b` | integer | Compressed byte length |
| `s` | bytes | Shard name |
| `n` | integer | Number of nonblank NDJSON records |

The compressed segment is a zlib stream containing newline-delimited UTF-8 JSON records.
A record line is at most 64 KiB. A segment is at most 64 MiB compressed. New publishers
write segment data in bounded chunks, normally no more than 8 MiB of text before
compression. The `s` descriptor value MUST agree with the shard on every decoded record.
Older v1 manifests may omit `b` and `s`; consumers of the current implementation retain
the documented legacy defaults (`s = "default"` and size inferred from a present file).

New media pack descriptors contain:

| Key | Type | Meaning |
| --- | --- | --- |
| `f` | bytes | Safe pack filename |
| `h` | bytes32 | SHA-256 of the complete pack |
| `b` | integer | Pack length, greater than zero and at most 16 MiB |
| `k` | bytes | `description` or `poster` |

A pack contains raw bytes and only one media kind. Record references use `description_ref`
or `poster_ref` objects with `pack`, `offset`, `length`, `sha256` and `type`. The reference
must remain within the named signed pack; its object hash and media type are checked before
serving. See [media-shards.md](media-shards.md) for the exact reference JSON, accepted MIME
types and API routes.

### Meta-list behavior

A meta-list is a signed collection of references, not a catalog shard. It cannot contain
record segments or media packs. Create one with `POST /api/publish` and
`{"name":"Curated collection","type":"meta"}`. Replace its pending entries with
`POST /api/publish/:pk/meta` and a body such as:

```json
{
  "items": [
    {"pubkey":"<64 lowercase hex>","annotation":"Feature films","enabled":true},
    {"pubkey":"<64 lowercase hex>","annotation":"Archived","enabled":false}
  ]
}
```

The publisher must already hold each referenced catalog and verify that it is not itself a
meta-list. A metadata-only `POST /api/publish/:pk/release` signs the staged items at the next
sequence. Subscribers automatically request enabled child lists; disabled entries remain
visible as signed annotations but are not followed. A child manifest that identifies itself
as a meta-list is rejected before its version is accepted. A meta-list already held locally
cannot be referenced as a child. Together these rules cap the graph at one meta-list level and
prevent recursive/cyclic subscription. A meta-list can still be subscribed to directly as a
root collection.

An optional `snapshot` integer records the previous sequence when a publisher compacts a
list. Normal releases append history. Snapshots rewrite the live logical contents into a
new compacted version and may remove obsolete tombstones and media objects.

## 7. Records, shards and deletion

Records are newline-delimited JSON objects. The minimum record identity is its torrent
infohash (`ih`) and title (`title`); optional metadata includes size, file count, date, tags,
trackers, legacy poster URL and media references. A legacy poster URL is metadata only and
MUST NOT be fetched automatically by a MediaMesh consumer.

A deletion is a tombstone with `del` containing the removed infohash and `shard` containing
the shard where the old record lived. A move between shards requires a tombstone in the old
shard and a new record in the destination shard. There is no implicit cross-shard deletion.

A shard name is `default` or 1–64 lowercase ASCII letters, digits, `_` or `-`. A consumer
selects complete shards, not arbitrary prefixes of a shard history. If any selected segment
of a shard is missing, that shard's records MUST NOT be indexed; otherwise a later missing
update or tombstone could expose stale state.

## 8. Validation, resource limits and security

A conforming consumer SHOULD enforce at least the current implementation limits:

- manifest: at most 4 MiB;
- segment descriptors: at most 4,096;
- media pack descriptors: at most 4,096;
- compressed record segment: at most 64 MiB;
- media pack: at most 16 MiB;
- decoded record line: at most 64 KiB;
- decoded records in one selected version: at most 1,000,000;
- aggregate decoded record text in one selected version: at most 256 MiB;
- media description: at most 64 KiB, valid UTF-8, NUL-free;
- poster: at most 2 MiB, JPEG/PNG/WebP with matching file signature.

A consumer MUST:

1. validate bencode shape, types, lengths and resource bounds before allocating based on
   untrusted sizes;
2. verify the BEP 44 pointer before starting a version fetch;
3. verify manifest signature, key, sequence and v2 infohash before trusting descriptors;
4. verify the SHA-256 and exact size of each downloaded segment/pack before installation;
5. reject unsafe paths, symlinks, duplicate descriptor paths and files not present in the
   signed allowlist;
6. preserve a per-list high-water sequence and reject rollback/equivocation;
7. avoid downloading record/media payload before manifest verification.

`have`, `known`, peer IDs, peer addresses, peer hints and DHT discovery results are
untrusted input. They can guide discovery but MUST NOT bypass cryptographic checks or
subscription policy. Only `have` contributes to the holder count; `known` is knowledge
gossip, not a popularity signal.

## Isolated local testing

A normal node joins the public MediaMesh directory rendezvous even when it is behind
NAT. NAT is not a privacy boundary. For tests that must not advertise catalog keys
or publish pointers to the public network, stop the daemon and set both startup
settings in its `settings.json`:

```json
{
  "DisableDHT": true,
  "DisableMeshDirectory": true
}
```

`DisableDHT` disables BEP 44 DHT get/put and DHT peer discovery;
`DisableMeshDirectory` prevents the fixed public directory torrent and its BEP 10
`have`/`known` exchange from starting. The local catalog API and local publishing
remain available. Restart the daemon after changing either setting. For normal
Internet-connected nodes, leave both settings false.

## 9. Current implementation boundaries

This document describes the existing P2Play profile, including its current limitations:

- Media lazy-fetch currently downloads a whole pack, not an exact object byte range.
- Directory `have` advertises locally held lists; it does not advertise shard or media
  availability. The separate `known` snapshot carries transitive knowledge and must never
  be counted as a holder or subscriber.
- Discovery-only lookup is implemented: a newly known key can request its signed BEP 44
  pointer, verify the key/sequence/Ed25519 signature and cache the bounded UTF-8 `v.n`
  display name (at most 64 bytes). `GET /api/lists/discover` exposes that name even before
  subscription. This path never subscribes and never downloads the version torrent,
  manifest, records or media. A later explicit subscription reuses the cached pointer.
- `holders` counts distinct peers advertising a local copy through `have`; `known_by`
  counts distinct direct peer IDs from which this node learned the key through either
  `have` or `known` (not a network-wide census). A `have` peer is also a knower, but
  a `known`-only peer never increments `holders`. The `known` inventory is propagated
  transitively. List ordering/ranking uses only `holders`, never `known_by`.
- The record index is local state; this document does not define a remote search RPC.
- DHT is optional; peer hints can bootstrap a DHT-disabled deployment when a directory peer
  is reachable.
- Power-loss durability, owner key rotation/recovery, global cross-list quotas and a
  cross-process lock on a shared data directory are not part of this version.

References:
- [Signed peer pointers](peer-items.md)
- [Media packs, shards and snapshots](media-shards.md)
- [Publisher transport and deployment](publisher-proxy.md)
