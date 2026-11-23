#!/usr/bin/env python3
"""Verify PublisherProxy under an OS-enforced Linux network-namespace kill switch.

The namespace has a default route through a veth so direct traffic would be
routable, but nftables permits only TCP to the SOCKS control endpoint and UDP
to Dante's bounded relay range. The test first proves direct TCP/UDP probes are
dropped, then exercises real P2Play DHT + peer traffic through Dante, stops
Dante, and verifies that the daemon cannot reach either fixture directly.

Requires Linux root via passwordless sudo, iproute2, nftables, dante-server,
and strace-free Python stdlib fixtures. Run only on a disposable CI/VM host.
"""
import argparse
import base64
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("proxy_fixture", HERE / "proxy-test.py")
FIXTURE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FIXTURE)

USERNAME = "proxy-netns-user"
PASSWORD = "proxy-netns-password"
TOKEN = "ts_proxy_netns_test"
BASIC = "Basic " + base64.b64encode(b"owner:test").decode()
RELAY_FIRST, RELAY_LAST = 30000, 30100


def command(argv, *, input_text=None, check=True, timeout=15):
    result = subprocess.run(
        argv, input=input_text, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, timeout=timeout,
    )
    if check and result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}): {' '.join(argv)}\n{result.stdout}")
    return result


class NetworkNamespace:
    def __init__(self):
        suffix = os.getpid() % 100000
        self.name = f"p2pns{suffix}"
        self.host_if = f"p2h{suffix}"
        self.ns_if = f"p2n{suffix}"
        octet = 1 + (os.getpid() % 240)
        self.host_ip = f"10.254.{octet}.1"
        self.ns_ip = f"10.254.{octet}.2"
        self.network = f"10.254.{octet}.0/30"
        self.table = f"p2pguard{suffix}"
        self.created = False

    @property
    def privileged(self):
        return [] if os.geteuid() == 0 else ["sudo", "-n"]

    def root(self, *args, **kwargs):
        return command(self.privileged + list(args), **kwargs)

    def inside(self, *args, **kwargs):
        return self.root("ip", "netns", "exec", self.name, *args, **kwargs)

    def setup(self, http_port, proxy_port):
        if os.geteuid() != 0 and not shutil.which("sudo"):
            raise RuntimeError("the namespace test requires root or sudo")
        for binary in ("ip", "nft", "setpriv"):
            if not shutil.which(binary):
                raise RuntimeError(f"{binary} is required for the namespace test")

        self.root("ip", "netns", "add", self.name)
        self.created = True
        self.root("ip", "link", "add", self.host_if, "type", "veth",
                  "peer", "name", self.ns_if)
        self.root("ip", "link", "set", self.ns_if, "netns", self.name)
        self.root("ip", "address", "add", self.host_ip + "/30", "dev", self.host_if)
        self.root("ip", "link", "set", self.host_if, "up")
        self.inside("ip", "address", "add", self.ns_ip + "/30", "dev", self.ns_if)
        self.inside("ip", "link", "set", "lo", "up")
        self.inside("ip", "link", "set", self.ns_if, "up")
        # Deliberately provide a default route: the firewall, not lack of a
        # route, must prevent direct peer/DHT traffic from leaving the namespace.
        self.inside("ip", "route", "add", "default", "via", self.host_ip)

        rules = f"""
table inet {self.table} {{
  chain input {{
    type filter hook input priority 0; policy drop;
    iifname "lo" accept
    ct state established,related accept
    ip saddr {self.host_ip} tcp dport {http_port} accept
  }}
  chain output {{
    type filter hook output priority 0; policy drop;
    oifname "lo" accept
    ct state established,related accept
    ip daddr {self.host_ip} tcp dport {proxy_port} accept
    ip daddr {self.host_ip} udp dport {RELAY_FIRST}-{RELAY_LAST} accept
    counter drop
  }}
}}
"""
        self.inside("nft", "-f", "-", input_text=rules)
        return self

    def drop_count(self):
        output = self.inside("nft", "-nn", "list", "chain", "inet", self.table, "output").stdout
        match = re.search(r"counter packets (\d+) bytes \d+ drop", output)
        if not match:
            raise RuntimeError("nftables output drop counter not found:\n" + output)
        return int(match.group(1))

    def run_as_runner(self, *args, **kwargs):
        uid, gid = os.getuid(), os.getgid()
        return self.inside(
            "setpriv", f"--reuid={uid}", f"--regid={gid}", "--clear-groups",
            *args, **kwargs,
        )

    def cleanup(self):
        if not self.created:
            return
        self.root("ip", "netns", "delete", self.name, check=False)
        self.root("link", "delete", self.host_if, check=False)
        self.created = False


class DanteServer:
    def __init__(self, netns, root):
        self.netns = netns
        self.directory = Path(root)
        self.config = self.directory / "danted-netns.conf"
        self.pidfile = self.directory / "danted-netns.pid"
        self.log_path = self.directory / "danted-netns.log"
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.bind((netns.host_ip, 0))
            self.port = sock.getsockname()[1]
        self.prefix = netns.privileged
        self.log = self.log_path.open("wb")
        self.proc = None
        self.config.write_text("\n".join([
            "logoutput: stderr",
            f"internal: {netns.host_ip} port = {self.port}",
            f"external: {netns.host_ip}",
            "user.privileged: root",
            "user.notprivileged: nobody",
            "clientmethod: none",
            "socksmethod: none",
            "client pass {",
            f"  from: {netns.ns_ip}/32 to: {netns.host_ip}/32",
            "}",
            "socks pass {",
            f"  from: {netns.ns_ip}/32 to: {netns.host_ip}/32",
            "  command: bind connect udpassociate",
            f"  udp.portrange: {RELAY_FIRST}-{RELAY_LAST}",
            "}",
            "socks pass {",
            f"  from: {netns.ns_ip}/32 to: {netns.host_ip}/32",
            "  command: bindreply udpreply",
            "}",
            "",
        ]), encoding="utf-8")
        verify = command(self.prefix + ["danted", "-V", "-f", str(self.config)],
                         check=False, timeout=10)
        if verify.returncode:
            self.log.close()
            raise RuntimeError("danted rejected namespace config:\n" + verify.stdout)
        self.proc = subprocess.Popen(
            self.prefix + ["danted", "-f", str(self.config), "-p", str(self.pidfile)],
            stdout=self.log, stderr=subprocess.STDOUT, start_new_session=True,
        )
        if not FIXTURE.wait(self.ready, 10):
            self.close()
            raise RuntimeError(
                "Dante TCP listener was not reachable from the namespace. "
                "Last readiness probe: " + getattr(self, "last_probe", "<not run>")
                + "\nDante log:\n" + self.log_path.read_text(errors="replace")
            )
        FIXTURE.check(True, "Dante listens on the namespace veth address")

    def ready(self):
        if self.proc.poll() is not None:
            return False
        # Probe only TCP reachability here. The full SOCKS5 negotiation and
        # UDP/TCP forwarding are verified later by P2Play itself; duplicating a
        # protocol client in this readiness loop adds no coverage.
        probe = (
            "import socket,sys\n"
            "s=socket.create_connection((sys.argv[1],int(sys.argv[2])),timeout=1.0)\n"
            "s.close()\n"
        )
        result = self.netns.run_as_runner(
            "python3", "-c", probe, self.netns.host_ip, str(self.port),
            check=False, timeout=4,
        )
        self.last_probe = result.stdout
        return result.returncode == 0

    def close(self):
        if self.proc is not None and self.proc.poll() is None:
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(self.proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                self.proc.wait(timeout=5)
        if self.log and not self.log.closed:
            self.log.close()


class NamespacedPublisher:
    def __init__(self, netns, daemon, root, http_port, peer_port, dht, proxy_port):
        self.netns = netns
        self.root = Path(root)
        self.data = self.root / "p2play-data"
        self.data.mkdir()
        self.http_port = http_port
        self.base = f"http://{netns.ns_ip}:{http_port}"
        settings = {
            "DisableUPNP": True,
            "PeersListenPort": peer_port,
            "P2PDhtBootstrap": f"{netns.host_ip}:{dht.port}",
            "PublisherProxy": {
                "Enabled": True,
                "Host": netns.host_ip,
                "Port": proxy_port,
            },
        }
        (self.data / "settings.json").write_text(json.dumps(settings), encoding="utf-8")
        (self.data / "auth.json").write_text(json.dumps({
            "users": [{
                "username": "owner",
                "password": "test",
                "api_token": TOKEN,
            }]
        }), encoding="utf-8")
        self.log = (self.data / "stdout.log").open("wb")
        self.proc = subprocess.Popen(
            self.netns.privileged + [
                "ip", "netns", "exec", self.netns.name,
                "setpriv", f"--reuid={os.getuid()}", f"--regid={os.getgid()}",
                "--clear-groups", daemon, "--data", str(self.data),
                "--port", str(http_port),
            ],
            stdout=self.log, stderr=subprocess.STDOUT, start_new_session=True,
        )
        if not FIXTURE.wait(self.ready, 15):
            self.close()
            raise RuntimeError("namespaced p2playd did not start:\n" + self.read_log()[-5000:])
        FIXTURE.check(True, "P2Play starts inside the filtered network namespace")

    def read_log(self):
        path = self.data / "stdout.log"
        return path.read_text(errors="replace") if path.exists() else ""

    def ready(self):
        if self.proc.poll() is not None:
            return False
        try:
            return self.call("/echo")[0] == 200
        except (OSError, urllib.error.URLError):
            return False

    def call(self, path, body=None):
        payload = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(self.base + path, data=payload)
        request.add_header("Authorization", BASIC)
        if body is not None:
            request.add_header("Content-Type", "application/json")
        try:
            response = urllib.request.build_opener(
                urllib.request.ProxyHandler({})
            ).open(request, timeout=5)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            raw = response.read()
            try:
                result = json.loads(raw)
            except ValueError:
                result = raw.decode(errors="replace")
            return response.status, result

    def add_peer(self, peer, suffix):
        magnet = (
            "magnet:?xt=urn:btih:" + suffix * 40
            + f"&x.pe={self.netns.host_ip}:{peer.port}"
        )
        status, body = self.call("/torrents", {"action": "add", "link": magnet})
        FIXTURE.check(status == 200, f"namespaced magnet {suffix[0]} accepted ({body})")

    def close(self):
        if self.proc.poll() is None:
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                self.proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait(timeout=5)
        if self.log and not self.log.closed:
            self.log.close()


def direct_probe(netns, host_ip, tcp_port, udp_port, dht):
    # This helper runs inside the namespace as the same unprivileged UID as P2Play.
    code = (
        "import socket,sys\n"
        "s=socket.socket(); s.settimeout(1.0)\n"
        "try: s.connect((sys.argv[1],int(sys.argv[2])))\n"
        "except OSError: sys.exit(0)\n"
        "s.close(); sys.exit(1)\n"
    )
    probe = netns.run_as_runner("python3", "-c", code, host_ip, str(tcp_port),
                                check=False, timeout=5)
    FIXTURE.check(probe.returncode == 0,
                  "firewall blocks a direct TCP connection to a peer fixture")
    udp_code = (
        "import errno,socket,sys\n"
        "s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)\n"
        "try:\n"
        " s.sendto(b'publisher-direct-egress-probe',(sys.argv[1],int(sys.argv[2])))\n"
        "except OSError as e:\n"
        " if e.errno not in (errno.EPERM, errno.EACCES): raise\n"
        " print('UDP direct probe denied by firewall:',e)\n"
        "s.close()\n"
    )
    netns.run_as_runner("python3", "-c", udp_code, host_ip, str(udp_port), timeout=5)
    time.sleep(0.3)
    FIXTURE.check(not dht.packets, "firewall blocks a direct UDP/DHT datagram")
    FIXTURE.check(netns.drop_count() >= 2,
                  "nftables drop counter records both blocked direct probes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("daemon", help="built p2playd executable")
    args = parser.parse_args()
    daemon = os.path.abspath(args.daemon)
    if not os.path.isfile(daemon):
        parser.error("p2playd executable not found")
    if os.geteuid() != 0 and not shutil.which("sudo"):
        parser.error("requires root or passwordless sudo")
    if not shutil.which("danted"):
        parser.error("danted not found; install Ubuntu package dante-server")
    root = Path(tempfile.mkdtemp(prefix="p2play-proxy-netns-"))
    netns = NetworkNamespace()
    dht = FIXTURE.DhtTrap()
    peer = FIXTURE.PeerTrap()
    peer_after = None
    proxy = None
    node = None
    success = False
    try:
        http_port = FIXTURE.free_port()
        peer_port = FIXTURE.free_port()
        netns.setup(http_port, peer_port)
        direct_probe(netns, netns.host_ip, peer.port, dht.port, dht)

        proxy = DanteServer(netns, root)
        node = NamespacedPublisher(
            netns, daemon, root, http_port, peer_port, dht, proxy.port,
        )
        FIXTURE.check(FIXTURE.wait(lambda: bool(dht.packets), 20),
                      "P2Play DHT query reaches the host fixture through Dante UDP ASSOCIATE")
        node.add_peer(peer, "e")
        FIXTURE.check(FIXTURE.wait(
            lambda: any(b"BitTorrent protocol" in packet for packet in peer.data), 20
        ), "P2Play peer handshake reaches the host fixture through Dante TCP CONNECT")

        # Kill Dante while P2Play and the namespace firewall remain active.
        proxy.close()
        proxy = None
        peer_after = FIXTURE.PeerTrap()
        before_dht = len(dht.packets)
        node.add_peer(peer_after, "f")
        time.sleep(4)
        FIXTURE.check(not peer_after.data,
                      "proxy outage cannot expose a direct BitTorrent peer connection")
        FIXTURE.check(len(dht.packets) == before_dht,
                      "proxy outage cannot expose direct DHT traffic")
        FIXTURE.check(netns.drop_count() >= 2,
                      "OS firewall remains active after proxy shutdown")
        print("PASS PublisherProxy Linux namespace/firewall kill switch", flush=True)
        success = True
    except Exception:
        if node:
            print("---- namespaced P2Play log ----", flush=True)
            print(node.read_log()[-12000:], flush=True)
        if proxy:
            print("---- Dante log ----", flush=True)
            path = root / "danted-netns.log"
            if path.exists():
                print(path.read_text(errors="replace")[-8000:], flush=True)
        try:
            print("---- nftables output chain ----", flush=True)
            print(netns.inside("nft", "-nn", "list", "chain", "inet",
                               netns.table, "output", check=False).stdout, flush=True)
        except Exception:
            pass
        raise
    finally:
        if node:
            node.close()
        if proxy:
            proxy.close()
        if peer_after:
            peer_after.close()
        peer.close()
        dht.close()
        netns.cleanup()
        if success and not os.environ.get("KEEP"):
            shutil.rmtree(root, ignore_errors=True)
        else:
            print("Namespace test files retained at: " + str(root), flush=True)


if __name__ == "__main__":
    main()
