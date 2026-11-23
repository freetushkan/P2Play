# MediaMesh meta-lists

Meta-lists are signed collections of references to ordinary catalog lists. They do not merge
torrent records into a new catalog: each referenced child retains its own Ed25519 identity,
manifest sequence and BitTorrent version.

## Publisher workflow

1. Subscribe to or create the child catalog lists on the publisher node.
2. Create a list with `POST /api/publish` using `{"name":"...","type":"meta"}`.
3. Replace the pending entries with `POST /api/publish/:pk/meta`:
   `{"items":[{"pubkey":"<64 hex>","annotation":"Feature films","enabled":true}]}`.
4. Release with `POST /api/publish/:pk/release`. A metadata-only release is valid and
   advances the signed sequence without creating record segments.

The full item array is replaced on each call. At most 256 entries are allowed. A key must
refer to a list already held by the publisher, must not be the meta-list's own key, and must
not identify another meta-list. Keys are canonical lowercase 64-digit hex. Annotations are
UTF-8 text of at most 2,048 bytes. An entry's `enabled` flag is signed along with its key and
annotation.

## Subscriber behavior

The daemon follows enabled children when a meta-list is imported or when it starts. Disabled
entries are still exposed to clients as annotations but are not followed. Child lists are
independent holdings: they keep their own selection policy, record/media budgets, key,
sequence, and version transfer.

The parent and child manifests are both authenticated. If a referenced key is already held
as a meta-list, the parent import is rejected. If the child is not yet held, it is fetched
metadata-first; a child manifest that is itself a meta-list is rejected before its version is
accepted. Therefore a meta-list may be used as a root collection, but its entries are always
ordinary leaf catalogs. A child cannot recursively introduce another meta-list and form a
cycle.

Disabling an entry prevents future automatic subscription after the updated parent version
is accepted. It does not delete a child catalog that the user already subscribed to directly
or that another enabled meta-list still references.

## API and client surface

`GET /api/lists` and `GET /api/publish/:pk/status` expose:

```json
{
  "type": "meta",
  "items": [
    {
      "pubkey": "<64 lowercase hex>",
      "annotation": "Feature films",
      "enabled": true
    }
  ]
}
```

The Silo MediaMesh plugin displays entry annotations and enable state. All enabled children
are subscribed by the daemon; the client does not need to fan out requests itself.

See [mediamesh-v1.md](mediamesh-v1.md#6-manifest-v1) for the signed bencode fields and
[roadmap.md](roadmap.md) for the remaining client integration work.
