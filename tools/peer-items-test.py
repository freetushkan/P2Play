#!/usr/bin/env python3
"""Signed BEP10 pointers without DHT, stdlib only.
Usage: python3 tools/peer-items-test.py /absolute/path/to/p2playd
Checks invalid pointers, push/get, missed-push reconnect, queue limits and relay using
actual BitTorrent wire connections.
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
    def __init__(self, port, advertised_port=None):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.sock.settimeout(5)
        self.sock.sendall(b"\x13BitTorrent protocol" + b"\0\0\0\0\0\x10\0\0" + DIRECTORY + b"-PI0001-" + os.urandom(12))
        assert self.exact(68)[28:48] == DIRECTORY
        self.send(0, {b"m": {b"mediamesh": 42}, b"p": advertised_port or port})
        self.their = None
        self.inbox = []
        end = time.monotonic() + 10
        while self.their is None and time.monotonic() < end:
            kind, value = self._receive_wire()
            if kind == 0:
                self.their = value.get(b"m", {}).get(b"mediamesh")
            else:
                # HAVE/GET may arrive immediately after the extension handshake.
                # Preserve them while the raw test peer finishes negotiation.
                self.inbox.append((kind, value))
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

    def _receive_wire(self):
        while True:
            length, = struct.unpack(">I", self.exact(4))
            assert length < 1024 * 1024
            if not length:
                continue
            body = self.exact(length)
            if body[0] == 20:
                return body[1], unben(body[2:])

    def receive(self):
        if self.inbox:
            return self.inbox.pop(0)
        return self._receive_wire()

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

    def known(self, key, seq=0):
        self.send(self.their, {b"known": [[key, seq]]})

    def known(self, key, seq=0):
        self.send(self.their, {b"known": [[key, seq]]})

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
        # Learn the public key without subscribing to it.
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
            probe.known(key, 1)
            probe.inject(item)
            time.sleep(2.3)  # idle worker has drained verification queue
            assert not b.lists(), "malformed/forged pointer was accepted"
            probe.no_item(key)
            probe.close()
        print("PASS invalid signatures, salt, seq and pointer bounds rejected")

        # Two independent directory neighbors: one introduces the key; B must
        # relay it to the second neighbor without subscribing or downloading data.
        observer = Peer(b.bt, a.bt)
        peers.append(observer)
        probe = Peer(b.bt, a.bt)
        peers.append(probe)
        probe.known(key, 1)
        probe.inject(first)
        discovered = wait(lambda: next((l for l in b.call("/api/lists/discover")[1]["lists"]
                                        if l["pubkey"] == pk and l["name"] == "Peer pointer"), None), 20)
        assert discovered["subscribed"] is False, "discovery-only pointer lookup subscribed to the list"
        assert discovered["holders"] == 0, "KNOWN gossip incorrectly inflated holder popularity"
        assert discovered["known_by"] >= 1, "known peer was not counted separately"
        assert not b.lists(), "discovery-only pointer lookup downloaded/imported the version"
        assert ben(probe.get(key)) == ben(first), "verified discovery pointer was not cached for relay"
        propagated = observer.wait(lambda v: any(k == key for k, seq in v.get(b"known", [])), 25)
        assert any(k == key for k, seq in propagated[b"known"]), "learned key was not gossiped to a second peer"
        print("PASS discovery-only signed name lookup and two-neighbor KNOWN gossip without subscribing")
        observer.close()
        peers.remove(observer)
        probe.close()
        peers.remove(probe)

        a.start()
        source = Peer(a.bt)
        peers.append(source)
        assert b.call(f"/api/lists/{pk}", {})[0] == 202
        wait(lambda: any(l["pubkey"] == pk and l["seq"] == 1 and l["net"].get("records_ready") for l in b.lists()), 60)
        assert b.call("/api/catalog/search?q=peer")[1]["results"]
        print("PASS later subscription reuses the cached pointer and downloads the v2 version")
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

        # B goes offline before the next release. It must recover the missed version
        # through a fresh BEP 10 get after restart, not rely on the lost push.
        probe.close()
        peers.remove(probe)
        b.stop()
        a.call(f"/api/publish/{pk}/records", raw=json.dumps({"ih": "a" * 40, "title": "Offline update"}).encode())
        assert a.call(f"/api/publish/{pk}/release", {})[1]["seq"] == 3
        source.wait(lambda v: any(k == key and seq == 3 for k, seq in v.get(b"have", [])))
        third = source.wait(lambda v: v.get(b"item", {}).get(b"k") == key and
                            v.get(b"item", {}).get(b"seq") == 3)[b"item"]

        b.start()
        # Restore the subscription and its peer hint, then connect a raw protocol
        # peer. This separates BEP10 get recovery from TCP discovery timing.
        assert b.call(f"/api/lists/{pk}", {"peers": [f"127.0.0.1:{a.bt}"]})[0] == 202
        # B must issue get for its locally held seq=2; answer with signed seq=3.
        # The advertised port points at A, which still seeds the version torrent.
        recovery = Peer(b.bt, a.bt)
        peers.append(recovery)
        request = recovery.wait(lambda v: v.get(b"get") == key, 20)
        assert request[b"seq"] == 2, f"expected get after seq=2, got {request!r}"
        recovery.send(recovery.their, {b"item": third})
        wait(lambda: any(l["pubkey"] == pk and l["seq"] == 3 and l["net"].get("records_ready") for l in b.lists()), 60)
        print("PASS reconnect get recovers the pointer missed while offline")
        recovery.close()
        peers.remove(recovery)

        a.stop()
        source.close()
        b.stop()
        b.start()
        relay_peer = Peer(b.bt)
        peers.append(relay_peer)
        assert ben(relay_peer.get(key)) == ben(third)
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
