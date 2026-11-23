# Publisher transport: SOCKS5 and isolated VPN deployment

`PublisherProxy` applies to **the whole core/libtorrent session**, including publisher
lists, media swarms, the directory swarm, DHT requests and TCP/uTP peers. It is not a
per-list proxy, a proxy for the local HTTP API, or a guarantee of anonymity. Keep the
HTTP API private and authenticate it. For an OS-enforced kill switch, use a network
namespace/firewall or WireGuard as below, in addition to application policy.

## Configuration

Stop the process, protect its data directory (`chmod 700 /var/lib/p2play` on Linux),
and edit `/var/lib/p2play/settings.json`. Merge this into the existing object; do not
discard other settings:

```json
{
  "PublisherProxy": {
    "Enabled": true,
    "Host": "192.0.2.10",
    "Port": 1080,
    "Username": "replace-locally",
    "Password": "replace-locally"
  },
  "P2PDhtBootstrap": "198.51.100.20:6881,198.51.100.21:6881"
}
```

The documentation addresses above are placeholders, not working services. Obtain
trusted, current numeric bootstrap addresses outside the publisher's runtime. Leave
`Username` and `Password` out for a no-authentication proxy. `Enabled` must be a JSON
boolean; `Port` an integer from 1 through 65535. Both credentials must be present and
nonempty, or both absent/empty; each is limited to 255 **bytes**. NUL is rejected.
Unknown proxy options are rejected rather than silently ignored.

The proxy endpoint must be a **numeric IPv4 address**. Hostnames are rejected so
connecting to the proxy cannot trigger local DNS. The pinned libtorrent 2.0.11 UDP
proxy implementation uses an IPv4 proxy interface and does not support IPv6 UDP
ASSOCIATE relay replies. This restriction is about the proxy endpoint, not an
assurance that the proxy supports IPv6 destinations. Never supply a URL, credentials
in `Host`, or `0.0.0.0`.

The proxy must implement RFC 1928 **UDP ASSOCIATE**, maintain the TCP control
connection for the lifetime of the association, and support TCP CONNECT. A working
TCP-only proxy is insufficient for DHT/uTP/UDP trackers. Tor, HTTP CONNECT proxies,
and typical `ssh -D` setups do not supply this UDP capability. The proxy's relay
address and UDP ports must be reachable through the deployment firewall.

Credentials are plaintext in `settings.json`: protect backups and directory access.
The core does not log configured credentials; the settings API omits both fields and
returns `CredentialsSet` instead. Omitted credentials in API read/modify/write updates
are preserved. POSIX replacement settings files are restricted to mode 0600; console
filesystems may ignore Unix modes. A failed settings-file replacement does not delete
the previous transport policy.

## Startup, failure, and reconfiguration semantics

* Absent `PublisherProxy` means the historical direct mode. `Enabled: false` is an
  explicit direct configuration. Do not mistake either for a proxy kill switch.
* A malformed/unreadable existing settings file, malformed proxy object, invalid
  endpoint/credentials, or proxied hostname DHT bootstrap **refuses startup**. There
  is no parse-error fallback to direct mode.
* Proxy settings, disabled LSD/UPnP/NAT-PMP, and the DNS-safe DHT bootstrap setting
  are installed **before constructing the libtorrent session**. This avoids a
  transient direct-network/default-DHT-bootstrap window.
* The transport policy is immutable for a running process, including when currently
  in direct mode. API attempts to enable, disable, replace, or change proxy
  credentials fail without saving the candidate configuration. A settings reset
  cannot disable an active proxy. Unrelated live updates continue to work and
  preserve credentials. The existing compatibility API maps validation exceptions
  to a non-2xx response (currently HTTP 500), not a structured HTTP 400/409.
* To change transport: stop the process, edit the file, then start a **new process**.
  To explicitly return to direct mode, remove `PublisherProxy` or set it to
  `{"Enabled": false}` while stopped. Editing the disk file while running does not
  change the session and may be overwritten by a subsequent settings update.
* A syntactically valid configuration starts without waiting for remote proxy
  availability. Successful `/echo` means the local service started, **not** that the
  proxy authenticated or supports UDP. Authentication errors, refusal, lack of UDP
  ASSOCIATE, and association loss leave affected traffic stalled/retrying, never
  direct. TCP peer traffic can still work **through the proxy** if only UDP is
  unsupported. There is no whole-process fatal shutdown on a remote UDP failure.
* DHT remains available over the proxy unless `DisableDHT` is set. The default
  libtorrent hostname bootstrap is cleared in proxy mode. Supply numeric
  `P2PDhtBootstrap` entries for discovery; an empty list does not bootstrap itself.
  IPv6 bootstrap addresses can use `[address]:port`. Explicit bootstrap entries in
  magnets, .torrent files, or resume metadata must also be numeric.

## DNS and protocol boundaries

The core sets `proxy_hostnames`, `proxy_peer_connections`, and
`proxy_tracker_connections`; DHT/uTP/UDP trackers use libtorrent's SOCKS UDP wrapping.
Proxy outage does not clear `proxy_type` or restore local transports. Local discovery,
router port mapping and NAT-PMP are disabled, and libtorrent's proxy interface does
not accept ordinary direct incoming peer connections.

There are important libtorrent/application bypasses which this implementation closes:

1. **DHT bootstrap hostnames** are locally resolved by libtorrent independently of
   `proxy_hostnames`. Configuration and torrent bootstrap nodes must be numeric;
   the default hostname bootstrap is not used.
2. **HTTP tracker non-compact peer replies** can contain peer hostnames which the
   pinned libtorrent resolves locally. Strict proxy mode therefore filters out
   HTTP/HTTPS/other non-UDP trackers and overrides metadata trackers with the safe
   UDP-only set. There is no fallback HTTP announce. UDP tracker hostnames are
   passed to the SOCKS relay for remote resolution.
3. The core's standalone **HTTP .torrent fetcher** is not part of libtorrent. HTTP
   and HTTPS torrent-link fetching are rejected before resolving/connecting in
   proxy mode. Upload metadata, import a local file, or use a magnet instead.

Web seeds handled by libtorrent retain its SOCKS hostname/peer proxy settings; the
standalone core HTTP fetcher above is different. Tracker/webseed schemes unsupported
by the build remain unsupported. A proxy hides the publisher's direct network
endpoint from peers, not content, timing, identities in signed items, or the proxy
operator. It does not encrypt the client-to-proxy hop: use a trusted local proxy or a
protected tunnel when the hop is untrusted.

The local HTTP control API is deliberately outside this policy. On Linux the app also
uses a UDP `connect()` to `1.1.1.1:53` solely to select its displayed local interface
address; it closes the socket without sending a DNS query or any UDP packet. The
syscall test permits that route-selection call, not traffic to that destination.

## Linux network namespace: proxy-only kill switch

Run these commands as an administrator on a disposable/test host first. They create
new objects, not flush existing firewall tables. Use a dedicated unprivileged
`p2play` user and keep that account unable to alter network namespaces/firewalls.
This example assumes a trusted UDP-capable SOCKS server reachable on the host at
`10.203.0.1:1080`, with its UDP relay also on `10.203.0.1`. Configure the server to bind
its relay to that address and restrict its clients to the namespace. An externally
reachable remote proxy needs explicit routing/host forward/NAT rules instead; do
not grant unrestricted forwarding as a shortcut.

```sh
ip netns add p2pub
ip link add pp-host type veth peer name pp-ns
ip link set pp-ns netns p2pub
ip address add 10.203.0.1/30 dev pp-host
ip link set pp-host up
ip -n p2pub address add 10.203.0.2/30 dev pp-ns
ip -n p2pub link set lo up
ip -n p2pub link set pp-ns up
# Deliberately no namespace default route.

ip netns exec p2pub nft -f - <<'NFT'
table inet publisher_guard {
  chain input {
    type filter hook input priority 0; policy drop;
    iifname "lo" accept
    ct state established,related accept
    ip saddr 10.203.0.1 tcp dport 8090 accept
  }
  chain output {
    type filter hook output priority 0; policy drop;
    oifname "lo" accept
    ct state established,related accept
    ip daddr 10.203.0.1 tcp dport 1080 accept
    # SOCKS UDP relay ports can be dynamic. Narrow this to the proxy's configured
    # relay range if available. No UDP destination outside the proxy is allowed.
    ip daddr 10.203.0.1 udp accept
    counter drop
  }
}
NFT

# PublisherProxy.Host=10.203.0.1, Port=1080 in the protected settings file.
ip netns exec p2pub runuser -u p2play -- \
  /usr/local/bin/p2playd --data /var/lib/p2play --port 8090
# The host can reach the API at http://10.203.0.2:8090 (authentication required).
```

Do not bind a recursive DNS service on the allowed proxy address/UDP ports. Prefer
a bounded SOCKS relay range and permit only that range if the server supports it.
If a SOCKS server returns a different relay IP, explicitly authorize that trusted IP
and its relay ports; do not remove the output policy. The namespace has no IPv6
route and its `inet` firewall drops non-loopback IPv6 output. No DNS service is
provided in this proxy-only namespace: bootstrap addresses are preconfigured numeric
values and destination names are handled by SOCKS.

Capture on `pp-host` while publishing/seeding, then stop the SOCKS service, reject
UDP ASSOCIATE, and use a wrong password separately. Expect no peer/DHT/DNS packets
to any non-proxy destination. The proxy's upstream traffic belongs to the proxy
process outside the namespace, not the publisher. Check both sides of the veth and
firewall drop counters; a successful application test alone is not a firewall test.

## WireGuard alternative (also useful without SOCKS)

A VPN is an alternative for deployments needing direct inbound torrent connectivity
or UDP support not available from their SOCKS service. SOCKS may also be combined
with it, but traffic then leaves the VPN to the SOCKS server, not directly to peers.
WireGuard does not by itself force all applications into its tunnel; use a namespace
and an interface-based kill switch.

Starting from a new `p2pub` namespace, or the management veth above **without the
proxy-only firewall**, use a provider/operator-issued key, numeric endpoint and tunnel
address. Keep keys in root-readable files, never shell arguments or logs:

```sh
# WireGuard's UDP socket is born in the host namespace, then the interface moves.
# Thus only encrypted transport can use the host's normal default route.
ip link add wg-pub type wireguard
wg set wg-pub private-key /etc/wireguard/publisher.key \
  peer REPLACE_WITH_PEER_PUBLIC_KEY \
  endpoint 203.0.113.40:51820 allowed-ips 0.0.0.0/0 persistent-keepalive 25
ip link set wg-pub netns p2pub
ip -n p2pub address add 10.99.0.2/32 dev wg-pub
ip -n p2pub link set wg-pub up
ip -n p2pub route replace default dev wg-pub

# If and only if the VPN provides IPv6, add its assigned IPv6 address, ::/0
# allowed-ips, and an IPv6 default route through wg-pub. Otherwise IPv6 stays blocked.
mkdir -p /etc/netns/p2pub
printf '%s\n' 'nameserver 10.99.0.1' > /etc/netns/p2pub/resolv.conf
# Replace 10.99.0.1 with a DNS server reachable INSIDE this VPN. No host DNS fallback.

ip netns exec p2pub nft -f - <<'NFT'
table inet publisher_vpn_guard {
  chain input {
    type filter hook input priority 0; policy drop;
    iifname "lo" accept
    ct state established,related accept
    iifname "wg-pub" accept
    # Optional management veth from the preceding example:
    iifname "pp-ns" ip saddr 10.203.0.1 tcp dport 8090 accept
  }
  chain output {
    type filter hook output priority 0; policy drop;
    oifname "lo" accept
    oifname "wg-pub" accept
    # Only replies to the deliberately exposed management API may leave via veth.
    oifname "pp-ns" ip daddr 10.203.0.1 tcp sport 8090 ct state established accept
    counter drop
  }
}
NFT

ip netns exec p2pub runuser -u p2play -- \
  /usr/local/bin/p2playd --data /var/lib/p2play --port 8090
```

Set `PublisherProxy` disabled/absent for VPN-only use. Do not retain the proxy-only
firewall table when using the VPN-only recipe: both tables would apply and the old
one would drop tunneled traffic. Stop the publisher before changing recipes; replace
only the table you created, not unrelated host firewall policy. Configure the VPN
provider's peer routes, DNS and any forwarded incoming torrent port separately. Use
an `ip netns exec ... tcpdump` capture, and bring `wg-pub` down or remove the peer
while the publisher is active: no plaintext torrent/DHT/DNS traffic may appear on
`pp-host` or the physical host interface. The host should show only WireGuard UDP to
the configured endpoint and intentional management API traffic.

## Tests and remaining verification

Build the normal Linux target and run:

```sh
python3 -I tools/proxy-test.py /absolute/path/to/p2playd --trace
```

A separate smoke test uses the real **Dante** SOCKS server (Ubuntu package
`dante-server`, binary `danted`) rather than the Python protocol fixture. Run it on
Linux after installing `dante-server` and `strace`:

```sh
python3 -I tools/proxy-dante-test.py /absolute/path/to/p2playd --trace
```

The script starts a temporary, loopback-only Dante server with a bounded UDP relay
port range, verifies a real SOCKS5 `UDP ASSOCIATE` and UDP request/response, then
starts P2Play with that server as `PublisherProxy`. It checks that the daemon's DHT
traffic reaches a local DHT fixture through UDP relay and a BitTorrent peer handshake
reaches a local peer fixture through TCP CONNECT. With `--trace`, the P2Play process
is audited: direct peer/DHT destinations must not appear; only the Dante control
endpoint and its configured client-facing UDP relay range are permitted (plus the
documented packet-free interface route lookup). This is a real SOCKS server smoke
test, not a provider-specific Internet connectivity test or a network-namespace
firewall test.

The deterministic fixture test has no Python libtorrent dependency. It starts
local-only SOCKS, peer and DHT fixtures and checks:

* malformed JSON, field types, endpoint/port limits, credential limits, NUL/unknown
  options, and DNS-unsafe DHT bootstrap fail startup;
* enabling/disabling/credential changes and reset cannot reconfigure a live session;
  failed updates are not persisted; stop/edit/restart can change transport;
* authenticated and unauthenticated UDP ASSOCIATE, relayed DHT queries and TCP
  BitTorrent peer handshakes;
* UDP tracker hostname destinations are carried in SOCKS packets; HTTP trackers are
  excluded; direct HTTP torrent fetch and hostname magnet DHT bootstrap are blocked;
* unreachable proxy, authentication rejection, unsupported UDP ASSOCIATE and dropped
  live association do not cause direct fallback;
* public settings and logs omit proxy credentials, unrelated settings preserve them;
* with `--trace`, outbound publisher network syscalls address only the proxy/control
  association and relay, apart from the packet-free app route-selection call.

`--keep` retains fixture logs/traces and settings containing **test** credentials.
Without `--trace`, routing is observed through fixtures but absence of arbitrary DNS
or other direct destinations is not independently audited.

The local fixture does **not** prove public DHT reachability, successful mutable-item
replication, full media/list transfer through a production proxy, sustained uTP
throughput, IPv6 peer routing, proxy recovery after long backoff, provider-specific
relay/NAT behavior, console builds, or a real WireGuard/netns deployment. Those need
operator tests with the actual service and packet captures. Run the existing mesh,
media and peer-item tests as separate protocol regression coverage; their direct
fixtures are not a SOCKS anonymity test. The namespace/WireGuard commands above are
operational recipes, not actions performed by the automated test.
