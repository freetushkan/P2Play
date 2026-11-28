# Bounded outbound HTTP(S) fetch

Remote torrent files and external posters use one core-owned fetch policy:
`Core::fetchUrl`. The platform host supplies only the network transport; it
does not follow redirects or make the SSRF decision.

## Limits and validation

| Consumer | Maximum response body |
| --- | ---: |
| Torrent metadata URL | 16 MiB |
| Poster URL (including a `torrs://` card poster) | 2 MiB |

The maximum is passed into the C API callback before the body is read, and is
checked again by the core. A torrent response must parse as valid libtorrent
metadata. A poster must have a supported JPEG, PNG, WebP or GIF signature.

Only public IPv4 destinations are eligible. For every request and redirect the
core resolves the hostname again, rejects loopback, private, link-local,
multicast and the reserved ranges covered by the outbound-fetch policy, then
pins the selected address into the transport. URLs with user information,
unsupported schemes, invalid ports and malformed redirect targets are rejected.
Redirects are handled one hop at a time (maximum four); HTTP may upgrade to
HTTPS, but an HTTPS chain cannot downgrade to HTTP.

## Platform transport

- Linux standalone uses libcurl and the system trust store.
- Android standalone uses an authenticated loopback bridge to Java
  `SSLSocket` and Android's system CA store.
- Switch standalone exports currently trusted certificates from libnx SSL.
- PS4 and PS5 standalone use the CA bundle shipped at `data/ca/cacert.pem`.
- pPlay embedded uses pPlay PR #17's callback, backed by the existing
  `Http::perform`, platform CA setup and `Io::tlsVerify()` preference. The
  callback passes the core's DNS pin, enforces the requested response bound,
  disables automatic redirects and bypasses configured HTTP proxies that
  would otherwise circumvent the pinned direct connection.

No TLS verification failure is converted into an insecure retry. If a platform
TLS transport is unavailable, HTTPS fetching fails closed; ordinary HTTP remains
available through the configured host transport or the bounded socket fallback.

## Regression coverage

- `tools/http-fetch-test.cpp` exercises the core policy with a deterministic
  transport callback: HTTP and HTTPS imports, DNS pinning on every redirect,
  HTTP-to-HTTPS upgrade, HTTPS downgrade rejection, failed certificate
  verification, response limits, loopback rejection and invalid torrent bytes.
- `tools/https-test.py` / `tools/https-transport-test.cpp` exercise the real
  standalone libcurl transport against a local test CA: trusted and untrusted
  certificates, hostname mismatch, explicit redirect handling, a bencoded
  torrent response, and 2 MiB / 16 MiB response limits.
- `tools/ui-test.py` verifies that native API torrent URL imports reject
  loopback targets.
