#!/usr/bin/env python3
"""Signed BEP10 pointers without DHT, stdlib only.
Usage: python3 tools/peer-items-test.py /absolute/path/to/p2playd
Checks invalid signatures/salt/seq/shapes, rollback/equivocation, publisher-offline
relay, peer hints and restart persistence using actual BitTorrent wire connections.
Set KEEP=1 to retain daemon logs on failure.
"""
import copy
import hashlib
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

TOKEN = "peer-items-local-test-token"
CONTENT = b"MediaMesh v1 directory: the nodes holding lists meet here.\n"


def ben(x):
    if isinstance(x, int):
        return b"i" + str(x).encode() + b"e"
    if isinstance(x, bytes):
        return str(len(x)).encode() + b":" + x
    if isinstance(x, list):
        return b"l" + b"".join(ben(i) for i in x) + b"e"
    if isinstance(x, dict):
        return b"d" + b"".join(ben(k) + ben(x[k]) for k in sorted(x)) + b"e"
    raise TypeError(type(x))


def unben(data):
    def read(i):
        c = data[i:i + 1]
        if c == b"i":
            e = data.index(b"e", i)
            return int(data[i + 1:e]), e + 1
        if c in (b"l", b"d"):
            out = []
            i += 1
            while data[i:i + 1] != b"e":
                value, i = read(i)
                out.append(value)
            return (dict(zip(out[::2], out[1::2])) if c == b"d" else out), i + 1
        e = data.index(b":", i)
        length = int(data[i:e])
        return data[e + 1:e + 1 + length], e + 1 + length
    value, end = read(0)
    assert end == len(data)
    return value


DIRECTORY = hashlib.sha1(ben({b"length": len(CONTENT), b"name": b"mediamesh.v1",
                             b"piece length": 16384, b"pieces": hashlib.sha1(CONTENT).digest()})).digest()


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Peer:
    """A raw BEP10 test peer. Its extension id differs from the daemon's id."""
    def __init__(self, port, advertised_port=None, source_ip=None):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(5)
        if source_ip:
            self.sock.bind((source_ip, 0))
        self.sock.connect(("127.0.0.1", port))
        self.sock.sendall(b"\x13BitTorrent protocol" + b"\0\0\0\0\0\x10\0\0" + DIRECTORY + b"-PI0001-" + os.urandom(12))
        assert self.exact(68)[28:48] == DIRECTORY
        self.send(0, {b"m": {b"mediamesh": 42}, b"p": advertised_port or port})
        self.their = None
        end = time.monotonic() + 10
        while self.their is None and time.monotonic() < end:
            kind, value = self.receive()
            if kind == 0:
                self.their = value.get(b"m", {}).get(b"mediamesh")
        assert self.their, "daemon did not negotiate mediamesh"

    def exact(self, count):
        out = b""
        while len(out) < count:
            chunk = self.sock.recv(count - len(out))
            if not chunk:
                raise EOFError("peer disconnected")
            out += chunk
        return out

    def send(self, kind, value):
        body = bytes((20, kind)) + ben(value)
        self.sock.sendall(struct.pack(">I", len(body)) + body)

    def receive(self):
        while True:
            length, = struct.unpack(">I", self.exact(4))
            assert length < 1024 * 1024
            if not length:
                continue
            body = self.exact(length)
            if body[0] == 20:
                return body[1], unben(body[2:])

    def wait(self, predicate, seconds=20):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            try:
                kind, value = self.receive()
            except socket.timeout:
                continue
            if kind == 42 and predicate(value):
                return value
        raise AssertionError("expected BEP10 message not received")

    def get(self, key, seq=-1):
        self.send(self.their, {b"get": key, b"seq": seq})
        return self.wait(lambda v: b"item" in v and v[b"item"].get(b"k") == key)[b"item"]

    def inject(self, item):
        key = item[b"k"]
        self.wait(lambda v: v.get(b"get") == key)
        self.send(self.their, {b"item": item})

    def push(self, item):
        # A push is authorized by an exact key/sequence in this peer's latest HAVE.
        self.send(self.their, {b"have": [[item[b"k"], item[b"seq"]]]})
        self.send(self.their, {b"item": item})

    def push_many(self, items):
        self.send(self.their, {b"have": [[item[b"k"], item[b"seq"]] for item in items]})
        for item in items:
            self.send(self.their, {b"item": item})

    def no_item(self, key):
        self.send(self.their, {b"get": key, b"seq": -1})
        self.sock.settimeout(.5)
        try:
            end = time.monotonic() + .5
            while time.monotonic() < end:
                kind, value = self.receive()
                assert kind != 42 or value.get(b"item", {}).get(b"k") != key, "invalid pointer cached/relayed"
        except socket.timeout:
            pass
        finally:
            self.sock.settimeout(5)

    def close(self):
        self.sock.close()


class Node:
    def __init__(self, daemon, root, name):
        self.http, self.bt = free_port(), free_port()
        self.data = os.path.join(root, name)
        os.makedirs(self.data)
        with open(os.path.join(self.data, "settings.json"), "w") as f:
            json.dump({"PeersListenPort": self.bt, "DisableDHT": True, "DisableUPNP": True}, f)
        with open(os.path.join(self.data, "auth.json"), "w") as f:
            json.dump({"users": [{"username": "owner", "password": "peer-test", "api_token": TOKEN}]}, f)
        self.daemon = daemon
        self.proc = None
        self.start()

    def start(self):
        log = open(os.path.join(self.data, "stdout.log"), "a")
        self.proc = subprocess.Popen([self.daemon, "--data", self.data, "--port", str(self.http)],
                                     stdout=log, stderr=subprocess.STDOUT)
        log.close()
        wait(lambda: self.call("/api/system/ping")[0] == 200, 20)

    def stop(self):
        if self.proc is not None:
            self.proc.terminate()
            self.proc.wait(timeout=30)
            self.proc = None

    def call(self, path, body=None, raw=None):
        data = raw if raw is not None else (None if body is None else json.dumps(body).encode())
        req = urllib.request.Request(f"http://127.0.0.1:{self.http}" + path, data=data)
        req.add_header("Authorization", "Bearer " + TOKEN)
        try:
            with urllib.request.urlopen(req, timeout=10) as response:
                return response.status, json.loads(response.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())

    def lists(self):
        return self.call("/api/lists")[1]["lists"]


def wait(test, seconds=30):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            value = test()
            if value:
                return value
        except (ConnectionError, OSError):
            pass
        time.sleep(.2)
    raise AssertionError("condition timed out")


def main():
    daemon = os.path.abspath(sys.argv[1])
    root = tempfile.mkdtemp(prefix="p2play-peer-items-")
    nodes = []
    peers = []
    try:
        a = Node(daemon, root, "a")
        nodes.append(a)
        pk = a.call("/api/publish", {"name": "Peer pointer"})[1]["pubkey"]
        key = bytes.fromhex(pk)
        a.call(f"/api/publish/{pk}/records", raw=json.dumps({"ih": "d" * 40, "title": "Peer Film"}).encode())
        assert a.call(f"/api/publish/{pk}/release", {})[0] == 200
        source = Peer(a.bt)
        peers.append(source)
        first = source.get(key)
        assert first[b"seq"] == 1 and first[b"salt"] == b"mediamesh.v1"
        assert len(first[b"sig"]) == 64 and len(first[b"v"][b"ih"]) == 32
        print("PASS owner creates/serves signed pointers with DHT disabled")
        source.close()
        a.stop()  # prevent automatic LSD discovery during forged-pointer probes
        b = Node(daemon, root, "b")
        nodes.append(b)
        assert b.call(f"/api/lists/{pk}", {})[0] == 202

        invalid = []
        for field, value in ((b"salt", b"wrong"), (b"seq", -1), (b"seq", "overflow"), (b"seq", 1 << 63),
                             (b"sig", b"x" * 64), (b"sig", b"x" * 63)):
            item = copy.deepcopy(first)
            item[field] = value.encode() if isinstance(value, str) else value
            invalid.append(item)
        item = copy.deepcopy(first)
        item[b"v"][b"n"] = b"x" * 65
        invalid.append(item)
        item = copy.deepcopy(first)
        item[b"v"][b"ih"] = b"x" * 31
        invalid.append(item)
        item = copy.deepcopy(first)
        item[b"v"][b"n"] = b"x" * 3000
        invalid.append(item)
        item = copy.deepcopy(first)
        del item[b"salt"]
        invalid.append(item)
        item = copy.deepcopy(first)
        item[b"v"][b"ih"] = bytes(32)
        invalid.append(item)
        for item in invalid:
            probe = Peer(b.bt, a.bt)
            probe.inject(item)
            time.sleep(2.3)  # idle worker has drained verification queue
            assert not b.lists(), "malformed/forged pointer was accepted"
            probe.no_item(key)
            probe.close()
        print("PASS invalid signatures, salt, seq and pointer bounds rejected")

        a.start()
        source = Peer(a.bt)
        peers.append(source)
        probe = Peer(b.bt, a.bt)
        peers.append(probe)
        probe.push(first)
        wait(lambda: any(l["pubkey"] == pk and l["seq"] == 1 and l["net"].get("records_ready") for l in b.lists()), 60)
        assert b.call("/api/catalog/search?q=peer")[1]["results"]
        relay = probe.get(key)
        assert ben(relay) == ben(first)
        print("PASS peer item downloads v2 list and relays original signed envelope")
        # A's version 2 is delivered over BEP10; the old pointer must not replace it.
        a.call(f"/api/publish/{pk}/records", raw=json.dumps({"ih": "e" * 40, "title": "New Peer Film"}).encode())
        assert a.call(f"/api/publish/{pk}/release", {})[1]["seq"] == 2
        # An established directory peer gets the new signed item as a push. The have
        # snapshot announcing seq=2 must arrive first; no get is sent by this test peer.
        source.wait(lambda v: any(k == key and seq == 2 for k, seq in v.get(b"have", [])))
        pushed = source.wait(lambda v: v.get(b"item", {}).get(b"k") == key and
                             v.get(b"item", {}).get(b"seq") == 2)
        second = pushed[b"item"]
        assert second[b"seq"] == 2, "release did not push its new signed pointer"
        # The old handle retires asynchronously. Empty staging directories not
        # represented by torrent metadata may report delete-failed, not deleted.
        wait(lambda: len(os.listdir(os.path.join(a.data, "lists", ".network"))) == 1
             and a.lists()[0]["net"].get("records_ready"), 30)
        probe.inject(second)
        wait(lambda: any(l["seq"] == 2 and l["net"].get("records_ready") for l in b.lists()), 60)
        probe.push(first)
        time.sleep(2.3)
        assert b.lists()[0]["seq"] == 2 and probe.get(key)[b"seq"] == 2
        print("PASS newer sequence accepted, rollback rejected")

        # B misses the next push while disconnected. On reconnect it must request
        # the current pointer with get rather than assuming HAVE is authoritative.
        probe.close()
        peers.remove(probe)
        a.call(f"/api/publish/{pk}/records", raw=json.dumps({"ih": "f" * 40, "title": "Offline update"}).encode())
        assert a.call(f"/api/publish/{pk}/release", {})[1]["seq"] == 3
        source.wait(lambda v: any(k == key and seq == 3 for k, seq in v.get(b"have", [])))
        third = source.wait(lambda v: v.get(b"item", {}).get(b"k") == key and v.get(b"item", {}).get(b"seq") == 3)[b"item"]

        # More than the global queue capacity is sent from one source IP, using
        # separate connections so the per-connection BEP 10 budget is respected.
        attackers = []
        for i in range(12):
            attacker = Peer(b.bt, a.bt, source_ip="127.0.0.2")
            peers.append(attacker)
            attackers.append(attacker)
            fake_items = []
            for j in range(16):
                fake_items.append({
                    b"k": hashlib.sha256(f"flood-{i}-{j}".encode()).digest(),
                    b"salt": b"mediamesh.v1", b"seq": 1, b"sig": b"x" * 64,
                    b"v": {b"ih": b"z" * 32, b"n": b"flood"},
                })
            attacker.push_many(fake_items)

        fallback = Peer(b.bt, a.bt, source_ip="127.0.0.3")
        peers.append(fallback)
        fallback.inject(third)  # B's reconnect get is answered despite the other source's flood
        wait(lambda: any(l["pubkey"] == pk and l["seq"] == 3 and l["net"].get("records_ready") for l in b.lists()), 60)
        print("PASS reconnect get and per-source queue fairness under pointer flood")

        a.stop()
        probe.close()
        b.stop()
        b.start()
        relay_peer = Peer(b.bt)
        peers.append(relay_peer)
        assert ben(relay_peer.get(key)) == ben(second)
        print("PASS received pointer survives restart without publisher or private seed")
        c = Node(daemon, root, "c")
        nodes.append(c)
        assert c.call(f"/api/lists/{pk}", {"peers": [f"127.0.0.1:{b.bt}"]})[0] == 202
        wait(lambda: any(l["pubkey"] == pk and l["seq"] == 3 and l["net"].get("records_ready") for l in c.lists()), 60)
        print("PASS third node subscribes via directory peer hint after publisher exits")
        print("PASS signed peer item network tests")
    finally:
        for peer in peers:
            peer.close()
        for node in nodes:
            node.stop()
        if os.environ.get("KEEP"):
            print("Logs:", root)
        else:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
