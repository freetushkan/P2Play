#!/usr/bin/env python3
"""Publisher SOCKS5 integration checks, using only the Python standard library.

  python3 -I tools/proxy-test.py /absolute/path/to/p2playd [--trace] [--keep]

The fixture is deliberately local-only, not an open proxy. --trace requires Linux
strace and audits the publisher's outbound socket destinations, including DNS.
This is not a replacement for the namespace/firewall test in publisher-proxy.md.
"""

import argparse
import base64
import glob
import hashlib
import json
import os
import re
import select
import shutil
import signal
import socket
import socketserver
import struct
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

USER = "proxy-fixture-user"
PASSWORD = "proxy-fixture-password"
BASIC = "Basic " + base64.b64encode(b"owner:test").decode()
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def check(condition, message):
    if not condition:
        raise AssertionError(message)
    print("OK   " + message, flush=True)


def wait(predicate, seconds=12):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if predicate():
            return True
        time.sleep(0.1)
    return False


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def local_ip():
    # Route discovery only: connect(UDP) sends no packet.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect(("192.0.2.1", 9))
        return s.getsockname()[0]


def exact(sock, length):
    out = b""
    while len(out) < length:
        block = sock.recv(length - len(out))
        if not block:
            raise EOFError()
        out += block
    return out


def address(sock, atyp):
    if atyp == 1:
        host = socket.inet_ntop(socket.AF_INET, exact(sock, 4))
    elif atyp == 4:
        host = socket.inet_ntop(socket.AF_INET6, exact(sock, 16))
    elif atyp == 3:
        host = exact(sock, exact(sock, 1)[0]).decode("ascii")
    else:
        raise ValueError("invalid SOCKS address type")
    return host, struct.unpack("!H", exact(sock, 2))[0]


def udp_address(data):
    if len(data) < 4 or data[:3] != b"\0\0\0":
        raise ValueError("fragmented or malformed UDP envelope")
    atyp = data[3]
    if atyp == 1:
        host, pos = socket.inet_ntop(socket.AF_INET, data[4:8]), 8
    elif atyp == 4:
        host, pos = socket.inet_ntop(socket.AF_INET6, data[4:20]), 20
    elif atyp == 3:
        pos = 5 + data[4]
        host = data[5:pos].decode("ascii")
    else:
        raise ValueError("invalid UDP address type")
    return host, struct.unpack("!H", data[pos:pos + 2])[0], data[pos + 2:]


def envelope(host, port, payload):
    return b"\0\0\0\1" + socket.inet_aton(host) + struct.pack("!H", port) + payload


def bencode(value):
    if isinstance(value, int):
        return b"i" + str(value).encode() + b"e"
    if isinstance(value, bytes):
        return str(len(value)).encode() + b":" + value
    if isinstance(value, list):
        return b"l" + b"".join(bencode(x) for x in value) + b"e"
    return b"d" + b"".join(bencode(k) + bencode(value[k]) for k in sorted(value)) + b"e"


def bdecode(data):
    def parse(pos):
        kind = data[pos:pos + 1]
        if kind == b"i":
            end = data.index(b"e", pos)
            return int(data[pos + 1:end]), end + 1
        if kind in (b"d", b"l"):
            out = {} if kind == b"d" else []
            pos += 1
            while data[pos:pos + 1] != b"e":
                key, pos = parse(pos)
                if kind == b"d":
                    value, pos = parse(pos)
                    out[key] = value
                else:
                    out.append(key)
            return out, pos + 1
        end = data.index(b":", pos)
        length = int(data[pos:end])
        return data[end + 1:end + 1 + length], end + 1 + length
    return parse(0)[0]


class ThreadedServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


class SocksProxy:
    def __init__(self, auth=True, reject_udp=False, reject_auth=False):
        self.auth = auth
        self.reject_udp = reject_udp
        self.reject_auth = reject_auth
        self.events = []
        self.udp_packets = []
        self.handshakes = 0
        self.authenticated = 0
        self.closed = threading.Event()
        self.sockets = []
        self.relay_ports = set()
        self.allowed = {"127.0.0.1", "127.0.0.2", local_ip()}
        self.aliases = {"tracker.proxy-test.invalid": local_ip()}
        owner = self

        class Handler(socketserver.BaseRequestHandler):
            def handle(self):
                try:
                    owner.handle(self.request)
                except (OSError, EOFError, ValueError, UnicodeError):
                    pass

        self.server = ThreadedServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def target(self, host):
        host = self.aliases.get(host, host)
        if host not in self.allowed:
            raise ValueError("fixture refuses non-local destination")
        return host

    def handle(self, sock):
        self.sockets.append(sock)
        sock.settimeout(3)
        version, count = exact(sock, 2)
        if version != 5:
            return
        methods = exact(sock, count)
        self.handshakes += 1
        method = 2 if self.auth else 0
        if method not in methods:
            sock.sendall(b"\5\xff")
            return
        sock.sendall(bytes((5, method)))
        if method == 2:
            version, length = exact(sock, 2)
            username = exact(sock, length)
            password = exact(sock, exact(sock, 1)[0])
            good = version == 1 and username == USER.encode() and password == PASSWORD.encode()
            if not good or self.reject_auth:
                sock.sendall(b"\1\1")
                return
            self.authenticated += 1
            sock.sendall(b"\1\0")
        version, command, reserved, atyp = exact(sock, 4)
        host, port = address(sock, atyp)
        self.events.append((command, host, port))
        if command == 3:
            if self.reject_udp:
                sock.sendall(b"\5\7\0\1" + b"\0" * 6)
                return
            relay = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            relay.bind(("127.0.0.1", 0))
            relay.settimeout(0.1)
            self.sockets.append(relay)
            relay_port = relay.getsockname()[1]
            self.relay_ports.add(relay_port)
            sock.sendall(b"\5\0\0\1" + socket.inet_aton("127.0.0.1") + struct.pack("!H", relay_port))
            threading.Thread(target=self.udp_loop, args=(relay,), daemon=True).start()
            sock.settimeout(0.2)
            while not self.closed.is_set():
                try:
                    if not sock.recv(1):
                        break
                except socket.timeout:
                    continue
            relay.close()
        elif command == 1:
            upstream = socket.create_connection((self.target(host), port), timeout=3)
            self.sockets.append(upstream)
            sock.sendall(b"\5\0\0\1" + socket.inet_aton("127.0.0.1") + b"\0\0")
            while not self.closed.is_set():
                ready, _, _ = select.select([sock, upstream], [], [], 0.2)
                for source in ready:
                    block = source.recv(65536)
                    if not block:
                        return
                    (upstream if source is sock else sock).sendall(block)
        else:
            sock.sendall(b"\5\7\0\1" + b"\0" * 6)

    def udp_loop(self, relay):
        client = None
        while not self.closed.is_set():
            try:
                packet, source = relay.recvfrom(65536)
                if client is None or source == client:
                    host, port, payload = udp_address(packet)
                    client = source
                    self.udp_packets.append((host, port, payload))
                    relay.sendto(payload, (self.target(host), port))
                else:
                    relay.sendto(envelope(source[0], source[1], packet), client)
            except socket.timeout:
                continue
            except (OSError, ValueError, IndexError):
                if self.closed.is_set():
                    break

    def close(self):
        if self.closed.is_set():
            return
        self.closed.set()
        self.server.shutdown()
        self.server.server_close()
        for sock in self.sockets:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            sock.close()


class PeerTrap:
    def __init__(self):
        self.data = []
        owner = self

        class Handler(socketserver.BaseRequestHandler):
            def handle(self):
                self.request.settimeout(3)
                try:
                    owner.data.append(exact(self.request, 68))
                except (OSError, EOFError):
                    pass

        self.server = ThreadedServer(("0.0.0.0", 0), Handler)
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()


class DhtTrap:
    def __init__(self):
        self.packets = []
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", 0))
        self.sock.settimeout(0.1)
        self.port = self.sock.getsockname()[1]
        self.closed = False
        threading.Thread(target=self.loop, daemon=True).start()

    def loop(self):
        while not self.closed:
            try:
                payload, source = self.sock.recvfrom(65536)
                self.packets.append(payload)
                query = bdecode(payload)
                if query.get(b"y") != b"q":
                    continue
                self.sock.sendto(bencode({b"t": query[b"t"], b"y": b"r", b"r": {
                    b"id": hashlib.sha1(b"proxy DHT fixture").digest(), b"nodes": b"", b"token": b"test"}}), source)
            except socket.timeout:
                continue
            except (OSError, ValueError, KeyError, IndexError):
                pass

    def close(self):
        self.closed = True
        self.sock.close()


class Publisher:
    def __init__(self, args, root, settings, invalid=False, path=None):
        self.port = free_port()
        self.path = path or tempfile.mkdtemp(prefix="node-", dir=root)
        self.trace = args.trace
        with open(os.path.join(self.path, "settings.json"), "w") as f:
            if isinstance(settings, str):
                f.write(settings)
            else:
                json.dump(settings, f)
        with open(os.path.join(self.path, "auth.json"), "w") as f:
            json.dump({"users": [{"username": "owner", "password": "test", "api_token": "ts_proxy_test"}]}, f)
        command = [args.daemon, "--data", self.path, "--port", str(self.port)]
        if self.trace:
            command = ["strace", "-ff", "-s", "256", "-e", "trace=network", "-o", self.path + "/trace"] + command
        self.log = open(self.path + "/stdout.log", "wb")
        self.proc = subprocess.Popen(command, stdout=self.log, stderr=subprocess.STDOUT, start_new_session=True)
        if invalid:
            try:
                result = self.proc.wait(timeout=8)
                check(result != 0, "invalid configuration refuses startup")
            except subprocess.TimeoutExpired:
                self.close()
                raise AssertionError("invalid configuration unexpectedly started")
            self.log.close()
        elif not wait(self.ready, 15):
            self.close()
            raise AssertionError("publisher did not start; inspect " + self.path)

    def ready(self):
        try:
            return self.call("/echo")[0] == 200
        except (OSError, urllib.error.URLError):
            if self.proc.poll() is not None:
                raise AssertionError("publisher exited; inspect " + self.path)
            return False

    def call(self, path, body=None):
        data = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(f"http://127.0.0.1:{self.port}" + path, data=data)
        request.add_header("Authorization", BASIC)
        if body is not None:
            request.add_header("Content-Type", "application/json")
        try:
            response = OPENER.open(request, timeout=5)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            raw = response.read()
            try:
                content = json.loads(raw)
            except ValueError:
                content = raw.decode(errors="replace")
            return response.status, content

    def add_peer(self, peer, suffix="a", tracker=None):
        magnet = "magnet:?xt=urn:btih:" + suffix * 40 + f"&x.pe={local_ip()}:{peer.port}"
        if tracker:
            magnet += "&tr=" + urllib.parse.quote(tracker, safe="")
        status, _ = self.call("/torrents", {"action": "add", "link": magnet})
        check(status == 200, "magnet accepted")

    def close(self):
        if self.proc.poll() is None:
            os.killpg(self.proc.pid, signal.SIGTERM)
            try:
                self.proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait(timeout=5)
        self.log.close()

    def no_credentials(self):
        for name in ("stdout.log", "p2play.log"):
            path = os.path.join(self.path, name)
            if os.path.isfile(path):
                content = open(path, "rb").read()
                check(USER.encode() not in content and PASSWORD.encode() not in content,
                      "proxy credentials absent from " + name)

    def audit(self, proxy_port, relay_ports):
        if not self.trace:
            return
        lines = []
        for name in glob.glob(self.path + "/trace*"):
            lines.extend(open(name).read().splitlines())
        check(bool(lines), "strace captured publisher networking")
        addresses = re.compile(r'sin_port=htons\((\d+)\), sin_addr=inet_addr\("([^"]+)"\)')
        forbidden = []
        for line in lines:
            if not any(name in line for name in ("connect(", "sendto(", "sendmsg(", "sendmmsg(")):
                continue
            for port, host in addresses.findall(line):
                port = int(port)
                if host == "127.0.0.1" and port in ({proxy_port} | set(relay_ports)):
                    continue
                # src/app/main.cpp route discovery only. UDP connect sends no packet;
                # any sendto/sendmsg to this address is still a failure.
                if "connect(" in line and host == "1.1.1.1" and port == 53:
                    continue
                forbidden.append(line)
            if "AF_INET6" in line:
                forbidden.append(line)
        check(not forbidden, "no direct peer/DHT/DNS destinations in publisher syscalls")
        if forbidden:
            raise AssertionError("unexpected destination in trace")


def config(proxy=None, dht=None):
    out = {"DisableUPNP": True, "PeersListenPort": free_port(), "P2PDhtBootstrap": ""}
    if proxy is not None:
        out["PublisherProxy"] = {"Enabled": True, "Host": "127.0.0.1", "Port": proxy.port}
        if proxy.auth:
            out["PublisherProxy"].update({"Username": USER, "Password": PASSWORD})
    if dht is not None:
        out["P2PDhtBootstrap"] = f"{local_ip()}:{dht.port}"
    return out


def invalid_startup(args, root):
    valid = {"Enabled": True, "Host": "127.0.0.1", "Port": 1080}
    invalid = ["{", "[]", {"PublisherProxy": None}, {"PublisherProxy": {}},
               {"PublisherProxy": dict(valid, Enabled="true")},
               {"PublisherProxy": dict(valid, Host="localhost")},
               {"PublisherProxy": dict(valid, Host="::1")},
               {"PublisherProxy": dict(valid, Host="0.0.0.0")},
               {"PublisherProxy": dict(valid, Port=0)},
               {"PublisherProxy": dict(valid, Port=65536)},
               {"PublisherProxy": dict(valid, Port=1080.5)},
               {"PublisherProxy": dict(valid, Username=USER)},
               {"PublisherProxy": dict(valid, Username=USER, Password="x" * 256)},
               {"PublisherProxy": dict(valid, Password=True)},
               {"PublisherProxy": dict(valid, Host="127.0.0.1\0ignored")},
               {"PublisherProxy": dict(valid, Udp=False)},
               {"PublisherProxy": valid, "P2PDhtBootstrap": "dht.proxy-test.invalid:6881"},
               {"PublisherProxy": valid, "P2PDhtBootstrap": "127.0.0.1:6881,"}]
    for value in invalid:
        node = Publisher(args, root, value, invalid=True)
        node.no_credentials()
    check(True, "strict malformed/type/endpoint/auth/bootstrap startup validation")


def healthy(args, root, auth):
    proxy, dht, peer = SocksProxy(auth=auth), DhtTrap(), PeerTrap()
    node = None
    try:
        settings = config(proxy, dht)
        node = Publisher(args, root, settings)
        check(wait(lambda: any(e[0] == 3 for e in proxy.events)), "SOCKS5 UDP ASSOCIATE requested")
        check(wait(lambda: any(p[1] == dht.port and b"1:q" in p[2] for p in proxy.udp_packets)),
              "DHT query carried in SOCKS5 UDP envelope")
        check(wait(lambda: bool(dht.packets)), "DHT fixture receives relayed query")
        node.add_peer(peer, tracker="http://tracker.proxy-test.invalid:9999/announce")
        check(wait(lambda: any(e[0] == 1 and e[2] == peer.port for e in proxy.events), 20),
              "TCP peer uses SOCKS5 CONNECT")
        check(wait(lambda: any(b"BitTorrent protocol" in p for p in peer.data), 20),
              "BitTorrent handshake traverses proxy")
        if auth:
            check(proxy.authenticated > 0, "SOCKS5 username/password authenticated")
        status, public = node.call("/settings", {"action": "get"})
        check(status == 200 and "Username" not in public["PublisherProxy"] and
              "Password" not in public["PublisherProxy"] and public["PublisherProxy"]["CredentialsSet"] == auth,
              "settings API redacts proxy credentials")
        check(node.call("/settings", {"action": "set", "sets": {"DownloadRateLimit": 512}})[0] == 200,
              "unrelated live setting succeeds")
        persisted = json.load(open(node.path + "/settings.json"))
        check(not auth or persisted["PublisherProxy"]["Password"] == PASSWORD,
              "redacted read/modify/write preserves credentials")
        before = open(node.path + "/settings.json", "rb").read()
        for update in ({"PublisherProxy": {"Enabled": False}},
                       {"PublisherProxy": dict(public["PublisherProxy"], Port=free_port())},
                       {"PublisherProxy": dict(public["PublisherProxy"], Password="different")},
                       {"P2PDhtBootstrap": "dht.proxy-test.invalid:6881"}):
            check(node.call("/settings", {"action": "set", "sets": update})[0] >= 400,
                  "unsafe live reconfiguration rejected")
            check(open(node.path + "/settings.json", "rb").read() == before,
                  "rejected configuration is not persisted")
        check(node.call("/settings", {"action": "def"})[0] >= 400,
              "reset cannot silently disable an active proxy")
        check(open(node.path + "/settings.json", "rb").read() == before, "rejected reset keeps settings")
        check(node.call("/torrents", {"action": "add", "link": "http://fetch.proxy-test.invalid/a.torrent"})[0] >= 400,
              "standalone HTTP fetch blocked before DNS")
        bad = "magnet:?xt=urn:btih:" + "b" * 40 + "&dht=bootstrap.proxy-test.invalid:6881"
        check(node.call("/torrents", {"action": "add", "link": bad})[0] >= 400,
              "magnet hostname DHT bootstrap rejected")
        # UDP tracker hostname must be passed to SOCKS, not getaddrinfo locally.
        tracker = DhtTrap()
        try:
            node.add_peer(peer, suffix="c", tracker=f"udp://tracker.proxy-test.invalid:{tracker.port}/announce")
            check(wait(lambda: any(host == "tracker.proxy-test.invalid" and port == tracker.port
                                   for host, port, _ in proxy.udp_packets), 20),
                  "UDP tracker hostname resolved by proxy")
        finally:
            tracker.close()
        check(not any(e[1] == "tracker.proxy-test.invalid" and e[2] == 9999 for e in proxy.events),
              "unsafe HTTP tracker excluded")
        node.close()
        node.no_credentials()
        node.audit(proxy.port, proxy.relay_ports)
    finally:
        if node:
            node.close()
        proxy.close()
        dht.close()
        peer.close()


def outage(args, root, mode):
    proxy = SocksProxy(reject_udp=mode == "reject-udp", reject_auth=mode == "reject-auth")
    dht, peer = DhtTrap(), PeerTrap()
    node = None
    try:
        settings = config(proxy, dht)
        if mode == "dead":
            proxy.close()
        node = Publisher(args, root, settings)
        if mode != "dead":
            check(wait(lambda: proxy.handshakes > 0), "proxy handshake attempted")
        if mode == "drop":
            check(wait(lambda: bool(dht.packets)), "DHT active before proxy disconnect")
            proxy.close()
            time.sleep(0.3)
            dht.packets.clear()
        if mode == "reject-udp":
            # TCP may still work through SOCKS5; UDP/DHT must not fall back.
            node.add_peer(peer)
            check(wait(lambda: any(e[0] == 1 for e in proxy.events), 20),
                  "TCP remains proxied when UDP ASSOCIATE is unsupported")
        else:
            node.add_peer(peer)
        time.sleep(3)
        check(not dht.packets, mode + ": no direct DHT fallback")
        if mode != "reject-udp":
            check(not peer.data, mode + ": no direct peer fallback")
        node.close()
        node.no_credentials()
        node.audit(proxy.port, proxy.relay_ports)
    finally:
        if node:
            node.close()
        proxy.close()
        dht.close()
        peer.close()


def direct_restart(args, root):
    node = Publisher(args, root, config())
    try:
        check(node.call("/settings", {"action": "set", "sets": {"PublisherProxy": {
            "Enabled": True, "Host": "127.0.0.1", "Port": 1080}}})[0] >= 400,
              "enabling a proxy on a direct live session requires restart")
        check("PublisherProxy" not in json.load(open(node.path + "/settings.json")),
              "failed direct-to-proxy change not persisted")
    finally:
        node.close()
    proxy = SocksProxy()
    restarted = None
    try:
        restarted = Publisher(args, root, config(proxy), path=node.path)
        check(wait(lambda: any(e[0] == 3 for e in proxy.events)),
              "stop/edit/restart enables SOCKS5 on the same data directory")
        restarted.close()
        restarted = Publisher(args, root, config(), path=node.path)
        check("PublisherProxy" not in restarted.call("/settings", {"action": "get"})[1],
              "stop/edit/restart can explicitly restore direct mode")
    finally:
        if restarted:
            restarted.close()
        proxy.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("daemon", help="p2playd executable")
    parser.add_argument("--trace", action="store_true", help="audit daemon syscalls with Linux strace")
    parser.add_argument("--keep", action="store_true", help="keep fixture logs and settings (contain test credentials)")
    args = parser.parse_args()
    args.daemon = os.path.abspath(args.daemon)
    if args.trace and not shutil.which("strace"):
        parser.error("--trace requires strace")
    root = tempfile.mkdtemp(prefix="p2play-proxy-")
    success = False
    try:
        invalid_startup(args, root)
        direct_restart(args, root)
        healthy(args, root, auth=True)
        healthy(args, root, auth=False)
        for mode in ("dead", "reject-auth", "reject-udp", "drop"):
            outage(args, root, mode)
        success = True
        print("PASS publisher proxy routing, validation, reconfiguration and outage checks", flush=True)
    finally:
        if success and not args.keep:
            shutil.rmtree(root)
        else:
            print("Fixture logs and traces: " + root, flush=True)


if __name__ == "__main__":
    main()
