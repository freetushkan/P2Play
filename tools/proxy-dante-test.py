#!/usr/bin/env python3
"""Exercise P2Play through a real Dante SOCKS5 server with TCP and UDP relay.

Usage:
  python3 -I tools/proxy-dante-test.py /path/to/p2playd --trace

Ubuntu CI installs dante-server. This complements proxy-test.py's deterministic
SOCKS fixture: the test here uses the real danted implementation and verifies
UDP ASSOCIATE delivery, DHT traffic, TCP CONNECT and (with --trace) no direct
publisher destination outside the SOCKS endpoint/UDP relay.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("proxy_fixture", HERE / "proxy-test.py")
FIXTURE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FIXTURE)


def exact(sock, count):
    result = bytearray()
    while len(result) < count:
        block = sock.recv(count - len(result))
        if not block:
            raise EOFError("SOCKS server closed the control connection")
        result.extend(block)
    return bytes(result)


def socks_address(sock, atyp):
    if atyp == 1:
        host = socket.inet_ntop(socket.AF_INET, exact(sock, 4))
    elif atyp == 4:
        host = socket.inet_ntop(socket.AF_INET6, exact(sock, 16))
    elif atyp == 3:
        length = exact(sock, 1)[0]
        host = exact(sock, length).decode("ascii")
    else:
        raise AssertionError("Dante returned an unsupported SOCKS address type")
    port = struct.unpack("!H", exact(sock, 2))[0]
    return host, port


class DanteServer:
    def __init__(self, executable, root):
        self.port = FIXTURE.free_port()
        self.auth = False
        # Constrain the client-facing UDP relay range so strace can distinguish
        # Dante's relay socket from direct DHT/peer destinations.
        self.udp_ports = set(range(30000, 30101))
        self.directory = Path(root)
        self.config = self.directory / "danted.conf"
        self.pidfile = self.directory / "danted.pid"
        self.log_path = self.directory / "danted.log"
        self.config.write_text(
            "\n".join([
                "logoutput: stderr",
                f"internal: 127.0.0.1 port = {self.port}",
                "external: 127.0.0.1",
                "user.privileged: root",
                "user.notprivileged: nobody",
                "clientmethod: none",
                "socksmethod: none",
                "client pass {",
                "  from: 127.0.0.1/32 to: 0.0.0.0/0",
                "}",
                "socks pass {",
                "  from: 0.0.0.0/0 to: 0.0.0.0/0",
                "  command: bind connect udpassociate",
                "  udp.portrange: 30000-30100",
                "}",
                "socks pass {",
                "  from: 0.0.0.0/0 to: 0.0.0.0/0",
                "  command: bindreply udpreply",
                "}",
                "",
            ]),
            encoding="utf-8",
        )
        self.log = open(self.log_path, "wb")
        prefix = [] if os.geteuid() == 0 else ["sudo", "-n"]
        self.prefix = prefix
        verify = subprocess.run(
            prefix + [executable, "-V", "-f", str(self.config)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        if verify.returncode:
            self.log.close()
            raise RuntimeError("danted rejected test config:\n" + verify.stdout)
        self.proc = subprocess.Popen(
            prefix + [executable, "-f", str(self.config), "-p", str(self.pidfile)],
            stdout=self.log, stderr=subprocess.STDOUT, start_new_session=True,
        )
        try:
            FIXTURE.check(FIXTURE.wait(self._ready, 10), "real Dante SOCKS5 server starts")
        except Exception:
            self.close()
            raise

    def _ready(self):
        if self.proc.poll() is not None:
            raise RuntimeError("danted exited: " + self.log_path.read_text(errors="replace"))
        try:
            with socket.create_connection(("127.0.0.1", self.port), timeout=0.3) as sock:
                sock.settimeout(0.3)
                sock.sendall(b"\x05\x01\x00")
                return exact(sock, 2) == b"\x05\x00"
        except (OSError, EOFError):
            return False

    def udp_probe(self, dht):
        """Independently prove UDP ASSOCIATE and relay delivery through Dante."""
        control = socket.create_connection(("127.0.0.1", self.port), timeout=5)
        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            control.settimeout(5)
            control.sendall(b"\x05\x01\x00")
            FIXTURE.check(exact(control, 2) == b"\x05\x00", "Dante negotiates SOCKS5 no-auth")
            # UDP ASSOCIATE with an unspecified client relay address.
            control.sendall(b"\x05\x03\x00\x01\x00\x00\x00\x00\x00\x00")
            version, reply, reserved, atyp = exact(control, 4)
            FIXTURE.check(version == 5 and reply == 0 and reserved == 0,
                          "real Dante accepts UDP ASSOCIATE")
            relay_host, relay_port = socks_address(control, atyp)
            FIXTURE.check(relay_port in self.udp_ports,
                          "Dante binds the UDP relay inside the configured range")
            if relay_host in ("0.0.0.0", "::"):
                relay_host = "127.0.0.1"

            transaction = b"dt"
            query = FIXTURE.bencode({
                b"t": transaction,
                b"y": b"q",
                b"q": b"ping",
                b"a": {b"id": b"R" * 20},
            })
            udp.bind(("127.0.0.1", 0))
            udp.sendto(FIXTURE.envelope(FIXTURE.local_ip(), dht.port, query),
                       (relay_host, relay_port))
            udp.settimeout(6)
            response = None
            deadline = time.monotonic() + 6
            while time.monotonic() < deadline:
                packet, _source = udp.recvfrom(65536)
                host, port, payload = FIXTURE.udp_address(packet)
                if payload.startswith(b"d") and b"1:y1:r" in payload:
                    response = (host, port, payload)
                    break
            FIXTURE.check(response is not None, "UDP payload and response traverse real Dante relay")
            FIXTURE.check(response[2].find(transaction) >= 0,
                          "UDP relay returns the matching DHT transaction")
        finally:
            udp.close()
            control.close()

    def close(self):
        proc = getattr(self, "proc", None)
        if proc is not None and proc.poll() is None:
            # The server runs in its own process group, so this also stops the
            # privileged danted child when started through sudo.
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                proc.wait(timeout=5)
        log = getattr(self, "log", None)
        if log and not log.closed:
            log.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("daemon", help="p2playd executable")
    parser.add_argument("--danted", default=shutil.which("danted") or "/usr/sbin/danted",
                        help="path to danted (default: search PATH or /usr/sbin/danted)")
    parser.add_argument("--trace", action="store_true",
                        help="audit P2Play network syscalls with Linux strace")
    args = parser.parse_args()
    if not args.danted:
        parser.error("danted not found; install Ubuntu package dante-server")
    if args.trace and not shutil.which("strace"):
        parser.error("--trace requires strace")
    args.daemon = os.path.abspath(args.daemon)

    root = tempfile.mkdtemp(prefix="p2play-proxy-dante-")
    dht = FIXTURE.DhtTrap()
    peer = FIXTURE.PeerTrap()
    server = None
    node = None
    try:
        server = DanteServer(args.danted, root)
        server.udp_probe(dht)

        # Now exercise the actual libtorrent session through the real SOCKS server.
        dht.packets.clear()
        node = FIXTURE.Publisher(args, root, FIXTURE.config(server, dht))
        FIXTURE.check(FIXTURE.wait(lambda: bool(dht.packets), 15),
                      "P2Play DHT bootstrap traverses real Dante UDP ASSOCIATE")
        node.add_peer(peer, suffix="e")
        FIXTURE.check(FIXTURE.wait(lambda: any(
            b"BitTorrent protocol" in handshake for handshake in peer.data
        ), 20), "P2Play TCP BitTorrent handshake traverses real Dante CONNECT")

        node.close()
        node.no_credentials()
        if args.trace:
            node.audit(server.port, server.udp_ports)
        print("PASS P2Play PublisherProxy with real Dante SOCKS5 TCP and UDP", flush=True)
    finally:
        if node:
            node.close()
        if server:
            server.close()
        dht.close()
        peer.close()
        if os.environ.get("KEEP"):
            print("Fixture directory retained: " + root)
        else:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
