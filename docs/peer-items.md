# Signed peer pointers (stage 3)

MediaMesh directory peers negotiate `mediamesh` through the BEP 10 extension
handshake. The local receive ID is 23; outgoing messages always use the ID
advertised by the other peer. Only the fixed directory swarm enables this
extension. Existing `have` messages remain compatible with older nodes, and
BEP 44 DHT lookup/publication remains enabled unless `DisableDHT` is set.

## Messages

Every message is a bencoded dictionary; strings below marked “bytes” are binary,
not hex. Unknown or malformed messages are ignored.

* `{"have": [[key, seq], ...]}`: up to 4096 keys (32 bytes), nonnegative integer
  versions. This is untrusted discovery information, not proof of a version.
* `{"get": key, "seq": seq}`: request a signed pointer newer than the requester's
  known sequence. `key` is 32 bytes; `seq` is an int64 at least -1. A missing
  pointer or one with sequence <= the requested sequence produces no response.
* `{"item": {"k": key, "salt": "mediamesh.v1", "seq": seq,
  "sig": signature, "v": {"ih": infohash, "n": name}}}`: `key` is 32 bytes,
  `signature` is 64 bytes, `infohash` is the nonzero 32-byte v2 torrent infohash,
  and `name` is at most 64 bytes. `seq` is a nonnegative signed int64.

The `item` envelope is the same signed object persisted in
`lists/<hex-key>/pointer.ben`. The BEP 44 signature covers canonical bencoding of
`v` with the exact salt and sequence; it does **not** sign the BEP 10 wrapper.
Verification uses libtorrent's BEP 44 verifier, including its salt/sequence
signing rules. No list private key is required for a relay. Legacy pointer files
containing only `v`, `sig`, `seq` are upgraded by supplying the known key and
salt and verifying the original signature before use.

An established peer also pushes a pointer when a held list advances. It sends
the updated `have` snapshot before the matching `item`; a receiver accepts a
push only when the key and sequence match that peer's latest valid `have`.
Replies to outstanding `get` requests remain accepted for compatibility and
for peers that have not yet exchanged a fresh inventory.

## Acceptance and limits

Items must be canonical, contain exactly the documented keys, have no trailing
bytes, fit a 2048-byte envelope and the BEP 44 1000-byte value limit. Decoding
is limited to depth 8 / 64 tokens. Names, hash/signature/key lengths, integer
kinds/ranges and the exact salt are checked before signature verification.
Duplicate keys, alternate encodings, invalid signatures and zero hashes fail
closed. Network messages have a 512-KiB outer cap; `get` has a 128-byte cap.
Replies to outstanding `get` requests are accepted. Push items also require an
exact key/sequence match in the sender's latest `have` snapshot.

Each connection has at most 32 pending requests, retries after 15 ticks, and
rotates across requested keys. Budgets per tick are 32 served gets, 32 pushed
items, 32 received items and two `have` updates. The shared incoming worker queue holds at most 128 items globally, at most 8
items per TCP source IP, and at most 32 DHT replies. Duplicate envelopes are
ignored while queued. The worker swaps the bounded vector under the mesh mutex,
then drains the detached batch round-robin by source outside the mutex. Peers
behind the same NAT share a quota. Discovery retains at most 4096 keys and
256 holder IDs per key. Outbound `have` and request snapshots are bounded to
4096 entries. Cached pointers are only kept for local/subscribed lists; the
untrusted directory cannot create unlimited download state.

Only locally held or explicitly subscribed keys can trigger downloads. A
verified pointer raises a per-key high-water sequence before fetching starts;
older pointers and a different value at the same sequence cannot replace it.
Completed older in-flight downloads are discarded if a newer pointer has
already won. The downloaded manifest must have the pointer's **key and sequence** and
then pass the normal list signature/hash/version validation. Accepted pointer
files and manifest sequences preserve the rollback floor across restart.
Unsigned `have` versions never raise the signed-pointer floor.

## Scheduling and bootstrap

BEP 10 callbacks only read short snapshots and enqueue work. They do not read
files, verify signatures, invoke session APIs or download torrents. The existing
joined idle worker drains the bounded queue, verifies pointers and invokes
`add_torrent` outside `m_meshMutex`. DHT replies use the same verification and
acceptance path. Manifest-first imports and per-file payload installation use that
worker too, rather than a detached thread that could outlive `Core`. Unselected shards
and lazy media packs stay at zero priority; selected complete shard histories become
searchable without waiting for `torrent_finished_alert`. See
[media-shards.md](media-shards.md) for the selective transfer/storage contract.

Owner pointers are created and persisted before DHT publication, even when the
DHT is disabled. A release exposes its new signed pointer before returning; reseeding
may finish asynchronously while the previous handle's isolated files retire. Both
successful and failed disk-delete completion alerts release that retirement gate, with
leftover working files cleaned by the worker. Accepted received pointers are held for
partial version sharing and immediately republished
to the DHT when enabled, and served to directory peers with their original
signature. Sharing never assumes that a partial holder has the entire torrent or uses
`seed_mode`. The original torrent info dictionary is retained across partial restarts.
Periodic 30-minute republication continues for both owners and
relays. The DHT publication callback never replaces a higher DHT sequence with
a lower cached one.

Subscription `peers` (`ip:port`, including `[IPv6]:port`) bootstrap both the
directory swarm and the version torrent. Up to 128 hints are used. For peer
items, the directory peer's address and BEP 10 `p` listening port are also kept
as a bounded version-torrent hint. Hints are not authority: signatures and
content hashes still decide acceptance. This permits an isolated network with
DHT disabled, provided an initial directory peer is reachable. It also permits
a third node to retrieve a relayed pointer and list after the publisher exits.

## Tests

```
python3 tools/peer-items-test.py /absolute/path/to/p2playd
python3 tools/meshnet-test.py /absolute/path/to/p2playd
```

The first test needs only Python's standard library and uses real BitTorrent
wire handshakes with a different negotiated extension ID. DHT is disabled on
all nodes. It exercises owner signing, malformed/signature/salt/sequence/shape
rejection, version download/search, unchanged signature relay, upgrades,
rollback protection, asynchronous old-generation cleanup after a release, persistence
after restart, and publisher-offline third-node subscription via a directory peer hint. Set `KEEP=1` to keep logs.
The existing meshnet test exercises the retained DHT path (and requires the
Python libtorrent binding).
