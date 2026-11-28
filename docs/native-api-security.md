# P2Play native API: authentication and stream links

Status: implementation in progress in PR #45. This document describes the intended
P2Play-owned contract; only the endpoints already present in the merged code and
the open PR should be treated as implemented.

## User token lifecycle

Each account owns one durable random API token in `auth.json`.

- User creation generates a token once.
- `POST /api/v1/auth/login` verifies username/password and returns the existing token.
  Repeated login does not rotate it.
- API clients send `Authorization: Bearer <token>`.
- The browser receives the same token in an HttpOnly `p2play_token` cookie.
- A user may reset their own token; an administrator may reset tokens of lower-rank
  accounts. Reset persists the new token and immediately invalidates the old one.
- Normal `/api/v1/auth/me` and user-list responses do not expose API tokens.
  Token material is returned only by login, user creation/reset, and the explicit
  stream-link creation operation.

The token is not an expiring login session. It is a long-lived per-user API key.
Password changes do not silently rotate it. Account deletion and banning revoke access.

## Stream URLs

A browser client requests a share URL for a specific zero-based file index using
`POST /api/v1/torrents/{hash}/files/{index}/share`. The response contains a relative
URL of the form:

```text
/api/v1/torrents/{hash}/files/{index}/stream?token=<user-api-token>
```

The client copies `location.origin + url` to the clipboard. This works with HTTP
players such as mpv that cannot complete an interactive browser login. A link is
scoped to the user's library and permissions; it does not grant access to other
users' torrents. Resetting that user's token revokes all previously copied links.

The streaming response sends `Referrer-Policy: no-referrer`. Link-generation
responses use `Cache-Control: no-store` and `Referrer-Policy: no-referrer`.
When an optional base path is configured, generated share URLs include it, and the
authentication cookie is scoped to that prefix.
Never write tokens into logs, torrent metadata, playlist files, or ordinary user
representations. Do not put tokens in redirects or HTML attributes.

## Native REST boundary

The P2Play API is versioned under `/api/v1`; the daemon does not expose the old
unversioned compatibility routes. MediaMesh catalog operations remain P2Play-owned
endpoints under the versioned API.
The Silo plugin is an external host integration: it searches subscribed MediaMesh
catalogs and hands a selected magnet to Silo through Silo's own host API. It does
not need P2Play to emulate Silo endpoints.

`torrs://` and `.torrs` are supported application data formats used by Android/pPlay
for metadata, poster and card exchange. `torrs://` links are parsed by the native add
route; when a poster URL is present and passes the server's outbound-fetch policy,
P2Play stores the image locally and exposes it through the authenticated same-origin
poster route.

## HTTPS poster fetching

Required transport properties:

- HTTPS with certificate-chain and hostname verification enabled by default.
- The pPlay embedded core reuses pPlay's existing libcurl HTTP transport and the
  selected network's TLS verification setting. It must not add another TLS stack
  or per-request client construction to the core.
- Standalone Linux/Switch/PS4/PS5 use the host libcurl HTTPS transport. Linux leaves
  CA selection to libcurl and the operating system trust store. Switch exports the
  currently trusted CA certificates from the console SSL service; PS4/PS5 use the
  bundled CA set that is also shipped by pPlay.
- Android standalone uses a loopback-only, per-start-token-authenticated bridge from
  p2playd to the Android system SSLSocket provider. The system trust store and
  hostname verifier are used; the native core still supplies the DNS-pinned address.
- Redirects are handled one hop at a time; each target is parsed, resolved and
  checked against the public-address policy before the next request.
- URL fetches remain bounded by the poster byte limit and timeout.
- Disabling TLS verification is an explicit user network setting, never an
  automatic fallback after certificate failure.
- Redirects are returned to the core without automatic following on every standalone
  host too. A host-provided callback may handle both HTTP and HTTPS so embedded pPlay
  can reuse its configured HTTP transport; certificate and hostname verification apply
  to HTTPS requests and are never disabled after a TLS error.


## Non-persistent preview

pPlay may inspect and stream a torrent before adding it to the user's persistent
library. It uses `POST /api/v1/torrents/preview` with the same link/title/poster/
category fields as a normal add. The server activates the torrent in memory and
grants a transient preview membership to the authenticated user; it does not
write the library record or trigger persistent poster extraction. The existing
`DELETE /api/v1/torrents/{hash}` operation first removes a matching transient
preview membership. If the user also owns a persistent library entry, that entry
is left intact. Transient preview access is lost on server restart.


## Optional HTTP base path

The standalone host accepts `--base-path /some_sub_dir/more_path/ololo`; embedded
hosts may set `p2play_config.http_prefix`. The value is normalized to a leading
slash and must consist of non-empty, plain URL path segments (letters, digits,
`_`, `-`, `.`, `~`; `.` and `..` segments are rejected). Multiple nested
segments are supported. With a prefix configured, all HTTP routes, UI redirects,
stream/poster URLs and login cookies use that path. The daemon does not also expose
the same routes at the unprefixed root; those requests return 404. A reverse proxy
must preserve the full prefix when forwarding requests.
