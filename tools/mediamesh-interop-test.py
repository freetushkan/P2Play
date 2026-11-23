#!/usr/bin/env python3
"""Live MediaMesh v1 interoperability test using an independent stdlib peer.

Usage: python3 -I tools/mediamesh-interop-test.py /path/to/p2playd
The reference peer implements BitTorrent framing and bencoding independently.
It exercises both directions: P2Play serves an item to the peer, then accepts
that same signed item from the peer and serves it back after importing the list.
"""
import importlib.util
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SPEC = importlib.util.spec_from_file_location(
    "mediamesh_reference_peer",
    os.path.join(HERE, "mediamesh_reference_peer.py"),
)
REFERENCE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REFERENCE)

ReferencePeer = REFERENCE.ReferencePeer
bdecode = REFERENCE.bdecode
bencode = REFERENCE.bencode
validate_get = REFERENCE.validate_get
validate_have = REFERENCE.validate_have
validate_item = REFERENCE.validate_item

TOKEN = "mediamesh-reference-test-token"


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait(test, seconds=30):
    end = time.monotonic() + seconds
    last_error = None
    while time.monotonic() < end:
        try:
            value = test()
            if value:
                return value
        except (ConnectionError, OSError) as error:
            last_error = error
        time.sleep(0.2)
    if last_error:
        raise AssertionError("condition timed out: " + str(last_error))
    raise AssertionError("condition timed out")


class Node:
    def __init__(self, daemon, root, name):
        self.http = free_port()
        self.bt = free_port()
        self.data = os.path.join(root, name)
        os.makedirs(self.data)
        with open(os.path.join(self.data, "settings.json"), "w", encoding="utf-8") as stream:
            json.dump({
                "PeersListenPort": self.bt,
                "DisableDHT": True,
                "DisableUPNP": True,
            }, stream)
        with open(os.path.join(self.data, "auth.json"), "w", encoding="utf-8") as stream:
            json.dump({
                "users": [{
                    "username": "owner",
                    "password": "reference-peer",
                    "api_token": TOKEN,
                }]
            }, stream)
        self.daemon = daemon
        self.proc = None
        self.start()

    def start(self):
        log = open(os.path.join(self.data, "stdout.log"), "a", encoding="utf-8")
        self.proc = subprocess.Popen(
            [self.daemon, "--data", self.data, "--port", str(self.http)],
            stdout=log, stderr=subprocess.STDOUT
        )
        log.close()
        wait(lambda: self.call("/api/system/ping")[0] == 200, 20)

    def stop(self):
        if self.proc is not None:
            self.proc.terminate()
            self.proc.wait(timeout=30)
            self.proc = None

    def call(self, path, body=None, raw=None):
        data = raw if raw is not None else (
            None if body is None else json.dumps(body).encode("utf-8")
        )
        request = urllib.request.Request(
            "http://127.0.0.1:" + str(self.http) + path, data=data
        )
        request.add_header("Authorization", "Bearer " + TOKEN)
        try:
            with urllib.request.urlopen(request, timeout=10) as response:
                return response.status, json.loads(response.read())
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read())

    def lists(self):
        return self.call("/api/lists")[1]["lists"]

    def log_tail(self):
        path = os.path.join(self.data, "stdout.log")
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as stream:
                return "".join(stream.readlines()[-60:])
        except OSError:
            return ""


def test_wire_fixtures():
    fixture_path = os.path.join(HERE, "mediamesh-v1-wire-fixtures.json")
    with open(fixture_path, "r", encoding="utf-8") as stream:
        fixtures = json.load(stream)

    for name in ("extension_handshake", "have", "get"):
        encoded = bytes.fromhex(fixtures[name]["bencoded_hex"])
        decoded = bdecode(encoded)
        assert bencode(decoded) == encoded, name + " fixture is not canonical"
    assert decoded[b"get"] == bytes.fromhex("11" * 32)
    assert decoded[b"seq"] == 6

    have = bdecode(bytes.fromhex(fixtures["have"]["bencoded_hex"]))
    entries = validate_have(have)
    assert entries == [[bytes.fromhex("11" * 32), 7]]
    handshake = bdecode(bytes.fromhex(fixtures["extension_handshake"]["bencoded_hex"]))
    assert handshake == {b"m": {b"mediamesh": 42}, b"p": 6881}
    print("PASS independent BEP10 canonical wire fixtures")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: mediamesh-interop-test.py /path/to/p2playd")
    daemon = os.path.abspath(sys.argv[1])
    root = tempfile.mkdtemp(prefix="p2play-mediamesh-interop-")
    nodes = []
    peers = []
    try:
        test_wire_fixtures()

        publisher = Node(daemon, root, "publisher")
        nodes.append(publisher)
        status, result = publisher.call("/api/publish", {"name": "Reference interop"})
        assert status == 201, result
        pubkey = result["pubkey"]
        key = bytes.fromhex(pubkey)
        record = json.dumps({"ih": "d" * 40, "title": "Reference interop item"}).encode()
        assert publisher.call("/api/publish/" + pubkey + "/records", raw=record)[0] == 200
        status, released = publisher.call("/api/publish/" + pubkey + "/release", {})
        assert status == 200 and released["seq"] == 1

        source = ReferencePeer(publisher.bt, advertised_port=publisher.bt)
        peers.append(source)
        _, have_message, _ = source.wait_for(
            lambda ext, message: ext == source.remote_extension_id and
            isinstance(message, dict) and b"have" in message,
            20
        )
        entries = validate_have(have_message)
        assert [entry for entry in entries if entry[0] == key and entry[1] == 1]
        print("PASS P2Play to independent peer: have snapshot")

        item, raw_item = source.request_item(key, -1)
        assert item[b"seq"] == 1 and item[b"salt"] == b"mediamesh.v1"
        assert len(raw_item) <= 2048 and bencode(bdecode(raw_item)) == raw_item
        print("PASS P2Play to independent peer: signed item reply to get")

        subscriber = Node(daemon, root, "subscriber")
        nodes.append(subscriber)
        status, _ = subscriber.call("/api/lists/" + pubkey, {})
        assert status == 202

        # The independent peer advertises the publisher's listening port. The
        # subscriber uses it as the source hint for the v2 version torrent.
        relay = ReferencePeer(subscriber.bt, advertised_port=publisher.bt)
        peers.append(relay)
        _, get_message, _ = relay.wait_for(
            lambda ext, message: ext == relay.remote_extension_id and
            isinstance(message, dict) and b"get" in message and message[b"get"] == key,
            20
        )
        requested_key, known_seq = validate_get(get_message)
        assert requested_key == key and known_seq < item[b"seq"]
        relay.reply_item(item)
        wait(lambda: any(
            card["pubkey"] == pubkey and card["seq"] == 1 and
            card["net"].get("records_ready")
            for card in subscriber.lists()
        ), 60)
        print("PASS independent peer to P2Play: solicited item import and v2 fetch")

        # The subscriber has now imported and cached the exact signed envelope.
        returned, returned_raw = relay.request_item(key, -1)
        assert returned == item and returned_raw == raw_item
        print("PASS P2Play to independent peer: relayed envelope is byte-identical")

        print("PASS MediaMesh v1 independent reference-peer interoperability")
    except Exception:
        for node in nodes:
            tail = node.log_tail()
            if tail:
                print("---- daemon log ----\n" + tail, file=sys.stderr)
        raise
    finally:
        for peer in peers:
            peer.close()
        for node in nodes:
            node.stop()
        if not os.environ.get("KEEP"):
            shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
