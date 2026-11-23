#!/usr/bin/env python3
"""A small, independent MediaMesh v1 BEP 10 reference peer.

This is test code, not production code. It intentionally uses only the Python
standard library and implements BitTorrent framing and bencoding itself. It
does not import or link any P2Play code.
"""
from collections import deque
import hashlib
import os
import socket
import struct
import time

DIRECTORY_CONTENT = b"MediaMesh v1 directory: the nodes holding lists meet here.\n"
DIRECTORY_INFOHASH = bytes.fromhex("18afe4a5bf3024b5ff70d391bfed0f11bc97c9c9")
EXTENDED_MESSAGE_ID = 20
LOCAL_EXTENSION_ID = 42
MAX_MESSAGE = 512 * 1024
MAX_ITEM = 2048
MAX_HAVE = 4096
MAX_SEQUENCE = (1 << 63) - 1


class WireError(ValueError):
    pass


def bencode(value):
    if isinstance(value, bool):
        raise TypeError("boolean is not a bencode integer")
    if isinstance(value, int):
        return b"i" + str(value).encode("ascii") + b"e"
    if isinstance(value, bytes):
        return str(len(value)).encode("ascii") + b":" + value
    if isinstance(value, list):
        return b"l" + b"".join(bencode(part) for part in value) + b"e"
    if isinstance(value, dict):
        if any(not isinstance(key, bytes) for key in value):
            raise TypeError("bencode dictionary keys must be bytes")
        return b"d" + b"".join(
            bencode(key) + bencode(value[key]) for key in sorted(value)
        ) + b"e"
    raise TypeError("unsupported bencode value: " + type(value).__name__)


def bdecode(data, max_depth=8, max_tokens=17000):
    data = bytes(data)
    pos = 0
    tokens = 0

    def read(depth):
        nonlocal pos, tokens
        tokens += 1
        if tokens > max_tokens:
            raise WireError("bencode token limit exceeded")
        if depth > max_depth or pos >= len(data):
            raise WireError("truncated or too-deep bencode value")
        marker = data[pos:pos + 1]

        if marker == b"i":
            end = data.find(b"e", pos + 1)
            if end < 0:
                raise WireError("unterminated integer")
            raw = data[pos + 1:end]
            if not raw or raw == b"-0" or (raw.startswith(b"0") and raw != b"0") or (
                raw.startswith(b"-0")
            ):
                raise WireError("noncanonical integer")
            digits = raw[1:] if raw.startswith(b"-") else raw
            if not digits.isdigit():
                raise WireError("invalid integer")
            pos = end + 1
            value = int(raw)
            if value < -(1 << 63) or value > MAX_SEQUENCE:
                raise WireError("integer outside signed 64-bit range")
            return value

        if marker == b"l":
            pos += 1
            result = []
            while True:
                if pos >= len(data):
                    raise WireError("unterminated list")
                if data[pos:pos + 1] == b"e":
                    pos += 1
                    return result
                result.append(read(depth + 1))

        if marker == b"d":
            pos += 1
            result = {}
            previous = None
            while True:
                if pos >= len(data):
                    raise WireError("unterminated dictionary")
                if data[pos:pos + 1] == b"e":
                    pos += 1
                    return result
                key = read(depth + 1)
                if not isinstance(key, bytes):
                    raise WireError("dictionary key is not a byte string")
                if previous is not None and key <= previous:
                    raise WireError("duplicate or unsorted dictionary key")
                previous = key
                result[key] = read(depth + 1)

        if marker.isdigit():
            colon = data.find(b":", pos)
            if colon < 0:
                raise WireError("unterminated byte-string length")
            raw_length = data[pos:colon]
            if not raw_length or (len(raw_length) > 1 and raw_length.startswith(b"0")):
                raise WireError("noncanonical byte-string length")
            if not raw_length.isdigit():
                raise WireError("invalid byte-string length")
            length = int(raw_length)
            start = colon + 1
            end = start + length
            if end > len(data):
                raise WireError("truncated byte string")
            pos = end
            return data[start:end]

        raise WireError("unknown bencode marker")

    result = read(0)
    if pos != len(data):
        raise WireError("trailing bytes after bencode value")
    return result


def validate_have(message):
    if not isinstance(message, dict) or set(message) != {b"have"}:
        raise WireError("expected a have-only dictionary")
    entries = message[b"have"]
    if not isinstance(entries, list) or len(entries) > MAX_HAVE:
        raise WireError("invalid have snapshot")
    seen = set()
    for entry in entries:
        if not isinstance(entry, list) or len(entry) != 2:
            raise WireError("invalid have entry")
        key, seq = entry
        if not isinstance(key, bytes) or len(key) != 32:
            raise WireError("invalid list key in have")
        if not isinstance(seq, int) or isinstance(seq, bool) or not 0 <= seq <= MAX_SEQUENCE:
            raise WireError("invalid sequence in have")
        if key in seen:
            raise WireError("duplicate list key in have")
        seen.add(key)
    return entries


def validate_get(message):
    if not isinstance(message, dict) or set(message) != {b"get", b"seq"}:
        raise WireError("expected get and seq only")
    key, seq = message[b"get"], message[b"seq"]
    if not isinstance(key, bytes) or len(key) != 32:
        raise WireError("invalid list key in get")
    if not isinstance(seq, int) or isinstance(seq, bool) or not -1 <= seq <= MAX_SEQUENCE:
        raise WireError("invalid known sequence in get")
    if len(bencode(message)) > 128:
        raise WireError("get exceeds 128-byte body limit")
    return key, seq


def validate_item(message):
    if not isinstance(message, dict) or set(message) != {b"item"}:
        raise WireError("expected an item-only dictionary")
    item = message[b"item"]
    if not isinstance(item, dict) or set(item) != {b"k", b"salt", b"seq", b"sig", b"v"}:
        raise WireError("invalid signed item envelope")
    key, salt, seq, sig, version = (
        item[b"k"], item[b"salt"], item[b"seq"], item[b"sig"], item[b"v"]
    )
    if not isinstance(key, bytes) or len(key) != 32:
        raise WireError("invalid item public key")
    if salt != b"mediamesh.v1":
        raise WireError("invalid item salt")
    if not isinstance(seq, int) or isinstance(seq, bool) or not 0 <= seq <= MAX_SEQUENCE:
        raise WireError("invalid item sequence")
    if not isinstance(sig, bytes) or len(sig) != 64:
        raise WireError("invalid item signature length")
    if not isinstance(version, dict) or set(version) != {b"ih", b"n"}:
        raise WireError("invalid item version value")
    if not isinstance(version[b"ih"], bytes) or len(version[b"ih"]) != 32 or not any(version[b"ih"]):
        raise WireError("invalid version infohash")
    if not isinstance(version[b"n"], bytes) or len(version[b"n"]) > 64:
        raise WireError("invalid list name")
    if len(bencode(version)) > 1000 or len(bencode(item)) > MAX_ITEM:
        raise WireError("item exceeds protocol size limit")
    return item


class ReferencePeer:
    """Raw TCP/BEP 10 peer speaking MediaMesh with a libtorrent daemon."""

    def __init__(self, port, advertised_port=None, timeout=10):
        self.sock = socket.create_connection(("127.0.0.1", int(port)), timeout=timeout)
        self.sock.settimeout(timeout)
        self.pending = deque()
        self.local_extension_id = LOCAL_EXTENSION_ID
        self.remote_extension_id = None
        peer_id = b"-MMREF01-" + os.urandom(11)
        handshake = b"\x13BitTorrent protocol" + b"\x00\x00\x00\x00\x00\x10\x00\x00"
        self.sock.sendall(handshake + DIRECTORY_INFOHASH + peer_id)
        response = self._exact(68)
        if response[:20] != b"\x13BitTorrent protocol" or response[28:48] != DIRECTORY_INFOHASH:
            self.close()
            raise WireError("remote BitTorrent handshake mismatch")

        local = {b"m": {b"mediamesh": LOCAL_EXTENSION_ID}}
        if advertised_port is not None:
            local[b"p"] = int(advertised_port)
        self._send_frame(0, local)

        deadline = time.monotonic() + timeout
        while self.remote_extension_id is None:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                self.close()
                raise TimeoutError("BEP 10 extension handshake not received")
            self.sock.settimeout(remaining)
            extension_id, payload, raw = self._receive_frame()
            if extension_id == 0:
                if not isinstance(payload, dict):
                    raise WireError("extension handshake is not a dictionary")
                mapping = payload.get(b"m")
                if not isinstance(mapping, dict):
                    raise WireError("extension handshake has no m dictionary")
                remote_id = mapping.get(b"mediamesh")
                if not isinstance(remote_id, int) or not 1 <= remote_id <= 255:
                    raise WireError("remote mediamesh extension ID is invalid")
                self.remote_extension_id = remote_id
            else:
                self.pending.append((extension_id, payload, raw))
        self.sock.settimeout(timeout)

    def _exact(self, length):
        chunks = []
        remaining = length
        while remaining:
            chunk = self.sock.recv(remaining)
            if not chunk:
                raise EOFError("peer disconnected")
            chunks.append(chunk)
            remaining -= len(chunk)
        return b"".join(chunks)

    def _send_frame(self, extension_id, message):
        body = bencode(message)
        if len(body) > MAX_MESSAGE:
            raise WireError("outgoing message too large")
        frame = bytes((EXTENDED_MESSAGE_ID, int(extension_id))) + body
        self.sock.sendall(struct.pack(">I", len(frame)) + frame)

    def send(self, message):
        if self.remote_extension_id is None:
            raise WireError("extension handshake is incomplete")
        self._send_frame(self.remote_extension_id, message)

    def _receive_frame(self):
        while True:
            length = struct.unpack(">I", self._exact(4))[0]
            if length == 0:
                continue
            if length > MAX_MESSAGE + 2:
                raise WireError("incoming BitTorrent frame too large")
            body = self._exact(length)
            if body[0] != EXTENDED_MESSAGE_ID or len(body) < 2:
                continue
            extension_id = body[1]
            encoded = body[2:]
            payload = bdecode(encoded, max_depth=8, max_tokens=17000)
            return extension_id, payload, encoded

    def wait_for(self, predicate, timeout=20):
        deadline = time.monotonic() + timeout
        for _ in range(len(self.pending)):
            entry = self.pending.popleft()
            if predicate(entry[0], entry[1]):
                return entry
            self.pending.append(entry)
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("expected MediaMesh message not received")
            self.sock.settimeout(remaining)
            entry = self._receive_frame()
            if predicate(entry[0], entry[1]):
                self.sock.settimeout(10)
                return entry
            self.pending.append(entry)

    def request_item(self, key, known_seq=-1):
        self.send({b"get": key, b"seq": known_seq})
        _, message, raw = self.wait_for(
            lambda ext, value: ext == self.local_extension_id and
            isinstance(value, dict) and b"item" in value and
            value[b"item"].get(b"k") == key
        )
        item = validate_item(message)
        return item, raw

    def reply_item(self, item):
        self.send({b"item": item})

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass
