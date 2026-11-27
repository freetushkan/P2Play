#!/usr/bin/env python3
"""Pluggable, bounded source adapters for the P2Play catalog grabber.

The transaction/state/merge layer stays in grabber_lib. Adapters only read an
independent source and stage normalized observations in the current run.
"""
from __future__ import annotations

import datetime as dt
from email.utils import parsedate_to_datetime
import html
import json
import lzma
from pathlib import Path
import re
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
from html.parser import HTMLParser
from typing import Any, Dict, List, Optional, Sequence, Tuple

import grabber_lib as g
from grabber_templates import TemplateError, parse_template


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        # Never forward an API key/query token to a redirect target.
        return None


def _http_bytes(url: str, timeout: int, limit: int = g.MAX_HTTP_BYTES) -> bytes:
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme not in {"http", "https"} or not parsed.hostname:
        raise g.SourceError("remote source URL must use HTTP or HTTPS")
    if parsed.username or parsed.password or parsed.fragment:
        raise g.SourceError("remote source URL must not embed credentials or fragments")
    opener = urllib.request.build_opener(_NoRedirect)
    request = urllib.request.Request(
        url, headers={"User-Agent": "P2Play-Grabber/1.0",
                      "Accept": "application/xml,application/rss+xml,*/*"})
    try:
        with opener.open(request, timeout=timeout) as response:
            length = response.headers.get("Content-Length")
            if length and int(length) > limit:
                raise g.SourceError("remote response exceeds the configured size limit")
            data = response.read(limit + 1)
    except urllib.error.HTTPError as exc:
        raise g.SourceError(f"remote source returned HTTP {exc.code}; redirects are not followed") from exc
    except (urllib.error.URLError, TimeoutError, OSError, ValueError) as exc:
        raise g.SourceError("remote source request failed") from exc
    if len(data) > limit:
        raise g.SourceError("remote response exceeds the configured size limit")
    return data


class _XzReader:
    """Streaming XZ reader supporting concatenated streams and valid stream padding."""
    _MAGIC = b"\xfd7zXZ\x00"
    _CHUNK = 64 * 1024

    def __init__(self, raw, memory_limit: int):
        self.raw = raw
        self.memory_limit = memory_limit
        self.decoder = self._new_decoder()
        self.input = bytearray()
        self.pending = bytearray()
        self.eof = False
        self.streams = 1

    def _new_decoder(self):
        return lzma.LZMADecompressor(
            format=lzma.FORMAT_AUTO, memlimit=self.memory_limit
        )

    def _start_next_stream(self) -> None:
        # LZMADecompressor stops at the end of one XZ stream. Its unused_data
        # contains bytes already read from the next stream or from stream padding.
        self.input.extend(self.decoder.unused_data)
        self.decoder = None
        padding_bytes = 0

        while True:
            if not self.input:
                chunk = self.raw.read(self._CHUNK)
                if not chunk:
                    if padding_bytes % 4:
                        raise g.SourceError("RuTracker XZ has invalid stream padding")
                    self.eof = True
                    return
                self.input.extend(chunk)

            # XZ stream padding is zero-filled and its total length must be
            # divisible by four. It may appear between streams or at EOF.
            first_nonzero = next(
                (index for index, value in enumerate(self.input) if value),
                len(self.input),
            )
            if first_nonzero:
                padding_bytes += first_nonzero
                del self.input[:first_nonzero]
            if not self.input:
                continue
            if padding_bytes % 4:
                raise g.SourceError("RuTracker XZ has invalid stream padding")

            while len(self.input) < len(self._MAGIC):
                chunk = self.raw.read(self._CHUNK)
                if not chunk:
                    raise g.SourceError(
                        "RuTracker XZ has trailing data or a truncated concatenated stream"
                    )
                self.input.extend(chunk)

            if self.input[:len(self._MAGIC)] != self._MAGIC:
                raise g.SourceError("RuTracker XZ stream has trailing data")

            self.decoder = self._new_decoder()
            self.streams += 1
            return

    def read(self, size: int = -1) -> bytes:
        size = self._CHUNK if size is None or size < 0 else min(max(1, size), self._CHUNK)
        while len(self.pending) < size and not self.eof:
            try:
                if self.decoder.needs_input:
                    if self.input:
                        compressed = bytes(self.input)
                        self.input.clear()
                    else:
                        compressed = self.raw.read(self._CHUNK)
                    if not compressed:
                        raise g.SourceError("RuTracker XZ stream is truncated")
                else:
                    compressed = b""
                self.pending.extend(
                    self.decoder.decompress(compressed, max_length=self._CHUNK)
                )
            except lzma.LZMAError as exc:
                detail = str(exc) or exc.__class__.__name__
                if "memory usage limit exceeded" in detail.casefold():
                    raise g.SourceError(
                        "RuTracker XZ requires more decoder memory than "
                        f"xz_memory_limit={self.memory_limit} bytes; increase the "
                        "source's xz_memory_limit (maximum 2 GiB)"
                    ) from exc
                raise g.SourceError(
                    f"RuTracker XZ decompression failed: {detail}"
                ) from exc

            if self.decoder.eof:
                self._start_next_stream()

        result = bytes(self.pending[:size])
        del self.pending[:size]
        return result

class _BoundedXmlReader:
    def __init__(self, stream, byte_limit: int):
        self.stream = stream
        self.byte_limit = byte_limit
        self.total = 0
        self.overlap = b""

    def read(self, size: int = -1) -> bytes:
        size = 64 * 1024 if size is None or size < 0 else min(max(1, size), 64 * 1024)
        data = self.stream.read(size)
        self.total += len(data)
        if self.total > self.byte_limit:
            raise g.SourceError("XML source exceeds the decompressed size limit")
        probe = (self.overlap + data).upper()
        if b"<!DOCTYPE" in probe or b"<!ENTITY" in probe:
            raise g.SourceError("XML DTD and entity declarations are not allowed")
        self.overlap = probe[-16:]
        return data


def _open_xml(path: Path, compressed_limit: int, expanded_limit: int, memory_limit: int):
    if not path.is_file():
        raise g.SourceError("RuTracker XML dump does not exist")
    if path.stat().st_size > compressed_limit:
        raise g.SourceError("RuTracker compressed XML exceeds the configured size limit")
    raw = path.open("rb")
    magic = raw.read(6)
    raw.seek(0)
    is_xz = magic.startswith(b"\xfd7zXZ\x00")
    if path.name.lower().endswith(".xz") and not is_xz:
        raw.close()
        raise g.SourceError("RuTracker .xz file has an invalid XZ signature")
    stream = _XzReader(raw, memory_limit) if is_xz else raw
    return raw, _BoundedXmlReader(stream, expanded_limit), is_xz


def _xml_name(tag: str) -> str:
    return tag.rsplit("}", 1)[-1].casefold()


def _direct_child(element: ET.Element, name: str) -> Optional[ET.Element]:
    wanted = name.casefold()
    return next((child for child in list(element) if _xml_name(child.tag) == wanted), None)


def _direct_text(element: ET.Element, name: str) -> str:
    child = _direct_child(element, name)
    return " ".join("".join(child.itertext()).split()) if child is not None else ""


class _MarkupText(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.parts: List[str] = []
        self.hidden = 0

    def handle_starttag(self, tag, attrs):
        if tag.lower() in {"script", "style"}:
            self.hidden += 1
        elif tag.lower() in {"p", "br", "div", "li", "tr", "h1", "h2", "h3"}:
            self.parts.append(" ")

    def handle_endtag(self, tag):
        if tag.lower() in {"script", "style"} and self.hidden:
            self.hidden -= 1
        elif tag.lower() in {"p", "br", "div", "li", "tr", "h1", "h2", "h3"}:
            self.parts.append(" ")

    def handle_data(self, data):
        if not self.hidden:
            self.parts.append(data)


def _plain_text(markup: str, limit: int = 2048) -> str:
    parser = _MarkupText()
    try:
        parser.feed(markup[:g.MAX_CSV_FIELD_BYTES])
        parser.close()
        return " ".join(" ".join(parser.parts).split())[:limit]
    except Exception:
        return html.unescape(re.sub(r"<[^>]{0,1024}>", " ", markup))[:limit]


def _rutracker_topic(element: ET.Element, state, run_id: str, source_id: str, source=None) -> Tuple[str, Dict[str, Any], List[str], Dict[str, Any]]:
    source = source or {}
    extras = source.get("extra_fields", {})
    topic_id = str(element.get("id", ""))
    nested = next((node for node in element.iter()
                   if node is not element and _xml_name(node.tag) == "torrent" and node.get("hash")), None)
    if nested is None:
        raise ValueError("topic has no nested torrent hash")
    ih = g.normalize_hash(nested.get("hash"))
    forum = _direct_child(element, "forum")
    forum_id = str(forum.get("id", "")) if forum is not None else ""
    forum_name = _direct_text(element, "forum")
    categories = [forum_name] if forum_name else []

    if _direct_child(element, "del") is not None:
        if getattr(state, "inventory_mode", False):
            record = {"del": ih, "shard": "inventory"}
            provenance = {
                "topic_id": topic_id, "deleted": True,
                "forum_id": forum_id, "forum": forum_name,
            }
            return ih, record, categories, provenance

        previous = state.staged_record(ih)
        if not previous:
            # After an acknowledged deletion staged_records no longer contains
            # this hash. Reuse the persisted source tombstone when a newer full
            # XML snapshot still carries the historical <del/> topic.
            for observation in state.observations_for(run_id, ih):
                if observation["source_id"] != source_id:
                    continue
                candidate = json.loads(observation["record_json"])
                if "del" in candidate:
                    previous = candidate
                    break
        if not previous or not previous.get("shard"):
            raise ValueError("deleted topic has no last-published shard; manual review required")
        record = {"del": ih, "shard": g._valid_shard(previous["shard"])}
        provenance = {"topic_id": topic_id, "deleted": True, "forum_id": forum_id, "forum": forum_name}
        return ih, record, categories, provenance

    from grabber_policy import forum_group
    group = forum_group(source, forum_id)
    if group and not getattr(state, "inventory_mode", False):
        categories = [group]

    title = g._clean_text(_direct_text(element, "title"), "title")
    if not title:
        raise ValueError("topic title is empty")
    unix_text = str(element.get("unixts", "")).strip()
    if unix_text:
        date = g._parse_date(unix_text)
    else:
        registered = str(element.get("registred_at", "")).strip()
        if registered:
            parsed = dt.datetime.strptime(registered, "%Y.%m.%d %H:%M:%S")
            date = int(parsed.replace(tzinfo=dt.timezone.utc).timestamp())
        else:
            date = None

    size_text = str(element.get("size", "")).strip()
    size = int(size_text) if size_text else None
    if size is not None and (size < 0 or size > 2**63 - 1):
        raise ValueError("topic size is outside the supported range")
    try:
        tracker_id = int(str(nested.get("tracker_id")))
    except (TypeError, ValueError) as exc:
        raise ValueError("tracker_id must be an integer") from exc
    if tracker_id < 1 or tracker_id > 9999:
        raise ValueError("tracker_id is outside the supported range")
    tracker_host = "bt.t-ru.org" if tracker_id == 1 else f"bt{tracker_id}.t-ru.org"
    tracker = f"http://{tracker_host}/ann?magnet"
    file_count = sum(1 for node in element.iter() if _xml_name(node.tag) == "file")
    record = g._canonical_source_record({
        "ih": ih, "title": title, "date": date, "size": size,
        "files": file_count if file_count else None,
        "tags": g._tags_for(categories, {}),
        "trackers": [tracker],
    }, {})
    old_versions = []
    for old in element.iter() if extras.get("history", True) else ():
        if _xml_name(old.tag) == "old":
            old_versions.append({"hash": old.get("hash"), "time": old.get("time"),
                                 "unixts": old.get("unixts"),
                                 "title": " ".join("".join(old.itertext()).split())[:512]})
    dup_hints = []
    for dup in element.iter() if extras.get("duplicate_hints", True) else ():
        if _xml_name(dup.tag) == "dup":
            dup_hints.append({"p": dup.get("p"), "text": " ".join("".join(dup.itertext()).split())[:256]})
    provenance = {
        "topic_id": topic_id, "forum_id": forum_id, "forum": forum_name,
        "registred_at": element.get("registred_at"), "unixts": unix_text,
        "tracker_id": tracker_id, "old": old_versions[:128], "dup_hints": dup_hints[:128],
    }
    if not extras.get("history", True):
        provenance.pop("old", None)
    if not extras.get("duplicate_hints", True):
        provenance.pop("dup_hints", None)
    content = _direct_text(element, "content") if extras.get("description", True) else ""
    if content:
        provenance["content_text"] = _plain_text(content)
    return ih, record, categories, provenance


def rutracker_source(source: Dict[str, Any], templates, state, run_id: str,
                     publisher: Dict[str, Any], quarantine_dir: Path) -> Dict[str, Any]:
    source = dict(source)
    source["_forum_groups"] = {str(value): name
        for name, ids in source.get("category_groups", {}).items() for value in ids}
    source_id = str(source["id"])
    if not source.get("path"):
        return {"status": "skipped", "reason": "operator-supplied RuTracker XML/XZ path is not configured",
                "fetched": 0, "parsed": 0, "accepted": 0, "filtered": 0, "rejected": 0, "duplicates": 0}
    path = Path(str(source["path"])).expanduser().resolve()
    compressed_limit = int(source.get(
        "max_compressed_bytes", g.DEFAULT_RUTRACKER_COMPRESSED_BYTES))
    expanded_limit = int(source.get(
        "max_expanded_bytes", g.DEFAULT_RUTRACKER_EXPANDED_BYTES))
    memory_limit = int(source.get(
        "xz_memory_limit", g.DEFAULT_RUTRACKER_XZ_MEMORY_BYTES))
    max_topics = int(source.get("max_topics", g.DEFAULT_RUTRACKER_MAX_TOPICS))
    if not 1 <= compressed_limit <= g.MAX_RUTRACKER_COMPRESSED_BYTES:
        raise g.SourceError("max_compressed_bytes must be between 1 byte and 16 GiB")
    if not 1 <= expanded_limit <= g.MAX_RUTRACKER_EXPANDED_BYTES:
        raise g.SourceError("max_expanded_bytes must be between 1 byte and 128 GiB")
    if not g.MIN_RUTRACKER_XZ_MEMORY_BYTES <= memory_limit <= g.MAX_RUTRACKER_XZ_MEMORY_BYTES:
        raise g.SourceError("xz_memory_limit must be between 16 MiB and 2 GiB")
    if not 1 <= max_topics <= g.MAX_RUTRACKER_TOPICS:
        raise g.SourceError("max_topics must be between 1 and 50,000,000")
    if not path.is_file() or path.stat().st_size > compressed_limit:
        raise g.SourceError("RuTracker dump is missing or exceeds the compressed size limit")
    digest = g._file_sha256(path)
    is_named_xz = path.name.lower().endswith(".xz")
    checkpoint = {"kind": "rutracker-xml-xz" if is_named_xz else "rutracker-xml",
                  "file_sha256": digest, "compressed_bytes": path.stat().st_size}
    from grabber_policy import policy_revision
    checkpoint["policy_sha256"] = policy_revision(source)
    previous = state.checkpoint(source_id)
    if (previous and previous.get("file_sha256") == digest
            and previous.get("policy_sha256") == checkpoint["policy_sha256"]):
        return {"status": "unchanged", "checkpoint": checkpoint,
                "fetched": 0, "parsed": 0, "accepted": 0, "filtered": 0, "rejected": 0, "duplicates": 0}

    include_ids = source.get("forum_include_ids", [])
    if not isinstance(include_ids, list) or any(not str(x).isdigit() for x in include_ids):
        raise g.SourceError("forum_include_ids must be an array of numeric forum IDs")
    include_ids = {str(x) for x in include_ids}
    counters = {"status": "success", "fetched": 0, "parsed": 0, "accepted": 0,
                "filtered": 0, "rejected": 0, "duplicates": 0, "conflicts": 0,
                "quarantined": 0, "quarantine_truncated": False, "checkpoint": checkpoint,
                "errors": [], "compressed_bytes": path.stat().st_size,
                "topic_limit": max_topics}
    quarantine = quarantine_dir / f"{source_id}.jsonl"
    raw = None
    try:
        raw = path.open("rb")
        magic = raw.read(6)
        raw.seek(0)
        is_xz = magic.startswith(b"\xfd7zXZ\x00")
        if is_named_xz and not is_xz:
            raise g.SourceError("RuTracker .xz file has an invalid XZ signature")
        xml_stream = _XzReader(raw, memory_limit) if is_xz else raw
        bounded = _BoundedXmlReader(xml_stream, expanded_limit)
        stack: List[ET.Element] = []
        for event, element in ET.iterparse(bounded, events=("start", "end")):
            if event == "start":
                stack.append(element)
                continue
            is_topic = _xml_name(element.tag) == "torrent" and bool(element.get("id"))
            if is_topic:
                counters["fetched"] += 1
                if counters["fetched"] > max_topics:
                    raise g.SourceError(
                        f"RuTracker XML exceeds configured max_topics limit ({max_topics:,})"
                    )
                source_ref = f"{path}#topic={element.get('id')}"
                forum = _direct_child(element, "forum")
                forum_id = str(forum.get("id", "")) if forum is not None else ""
                forum_name = _direct_text(element, "forum")
                if forum_name:
                    state.add_category(run_id, source_id, forum_name, forum_id or None)
                try:
                    # Forum selection happens before expensive topic normalization
                    # (file counting, old/dup traversal and description extraction).
                    if include_ids and forum_id not in include_ids:
                        counters["filtered"] += 1
                    else:
                        ih, record, categories, provenance = _rutracker_topic(
                            element, state, run_id, source_id, source)
                        forum_id = str(provenance.get("forum_id", ""))
                        outcome = state.add_observation(
                            run_id, source_id, ih, int(source.get("priority", 30)),
                            record, categories, source_ref, digest, provenance)
                        counters["parsed"] += 1
                        if outcome == "accepted":
                            counters["accepted"] += 1
                        elif outcome == "duplicate":
                            counters["duplicates"] += 1
                        elif outcome == "provenance":
                            counters["provenance_updated"] = counters.get("provenance_updated", 0) + 1
                        else:
                            counters["conflicts"] += 1
                            reason = "conflicting duplicate infohash in RuTracker XML"
                            if len(counters["errors"]) < g.MAX_ROW_ERRORS:
                                counters["errors"].append({"source_ref": source_ref, "error": reason})
                            if g._quarantine(quarantine, source_id, source_ref, reason, {"ih": ih}):
                                counters["quarantined"] += 1
                            else:
                                counters["quarantine_truncated"] = True
                except (ValueError, TypeError, OverflowError, KeyError) as exc:
                    counters["rejected"] += 1
                    if len(counters["errors"]) < g.MAX_ROW_ERRORS:
                        counters["errors"].append({"source_ref": source_ref, "error": str(exc)})
                    if g._quarantine(quarantine, source_id, source_ref, str(exc),
                                     {"topic_id": element.get("id")}):
                        counters["quarantined"] += 1
                    else:
                        counters["quarantine_truncated"] = True
                if len(stack) >= 2:
                    try:
                        stack[-2].remove(element)
                    except ValueError:
                        pass
                element.clear()
            if stack:
                stack.pop()
        checkpoint["compression"] = "xz" if is_xz else "none"
        counters["checkpoint"] = checkpoint
    except ET.ParseError as exc:
        raise g.SourceError("RuTracker XML is malformed") from exc
    except (lzma.LZMAError, EOFError, OSError) as exc:
        raise g.SourceError("RuTracker XML/XZ could not be read or decompressed") from exc
    finally:
        if raw is not None:
            raw.close()
    return counters


def _torznab_url(endpoint: str, params: Dict[str, Any], api_key: str = "") -> str:
    parsed = urllib.parse.urlsplit(endpoint)
    if parsed.scheme not in {"http", "https"} or not parsed.hostname or parsed.username or parsed.password or parsed.fragment:
        raise g.SourceError("Torznab endpoint must be HTTP(S) without embedded credentials")
    pairs = [(k, v) for k, v in urllib.parse.parse_qsl(parsed.query, keep_blank_values=True)
             if k.casefold() not in {"apikey", "api_key"}]
    pairs.extend((str(k), str(v)) for k, v in params.items() if v is not None)
    if api_key:
        pairs.append(("apikey", api_key))
    return urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, parsed.path, urllib.parse.urlencode(pairs), ""))


def _xml_parse(data: bytes, context: str) -> ET.Element:
    if len(data) > g.MAX_HTTP_BYTES or re.search(br"<!\s*(?:DOCTYPE|ENTITY)", data, re.I):
        raise g.SourceError(f"{context} XML exceeds limits or contains DTD/entities")
    try:
        return ET.fromstring(data)
    except ET.ParseError as exc:
        raise g.SourceError(f"{context} XML is malformed") from exc


def _torznab_caps(data: bytes) -> Dict[str, str]:
    root = _xml_parse(data, "Torznab capabilities")
    result = {}
    for node in root.iter():
        # Torznab caps uses nested <category>/<subcat> nodes; both IDs are
        # valid category values returned on RSS items.
        if _xml_name(node.tag) in {"category", "subcat"} and node.get("id"):
            result[str(node.get("id"))] = g._clean_text(node.get("name", ""), "category", 256)
    return result


def _rss_items(data: bytes, categories: Optional[Dict[str, str]] = None) -> List[Dict[str, Any]]:
    root = _xml_parse(data, "RSS/Torznab")
    category_map = categories or {}
    result: List[Dict[str, Any]] = []
    for item in root.iter():
        if _xml_name(item.tag) != "item":
            continue
        guid = ""
        title = ""
        published = ""
        timestamp = None
        try:
            guid = next((" ".join("".join(child.itertext()).split()) for child in list(item)
                         if _xml_name(child.tag) == "guid"), "")
            title = next((" ".join("".join(child.itertext()).split()) for child in list(item)
                          if _xml_name(child.tag) == "title"), "")
            published = next((" ".join("".join(child.itertext()).split()) for child in list(item)
                              if _xml_name(child.tag) == "pubdate"), "")
            if published:
                try:
                    parsed = parsedate_to_datetime(published)
                    if parsed.tzinfo is None:
                        parsed = parsed.replace(tzinfo=dt.timezone.utc)
                    timestamp = int(parsed.timestamp())
                except (ValueError, TypeError, OverflowError):
                    timestamp = g._parse_date(published)
            attrs: Dict[str, List[str]] = {}
            for node in item.iter():
                if _xml_name(node.tag) == "attr":
                    name, value = str(node.get("name", "")).casefold(), str(node.get("value", ""))
                    if name and value:
                        attrs.setdefault(name, []).append(value)
            def attr(name: str) -> str:
                values = attrs.get(name.casefold(), [])
                return values[0] if values else ""
            enclosure = next((child for child in list(item) if _xml_name(child.tag) == "enclosure"), None)
            magnet = attr("magneturl")
            if not magnet and enclosure is not None and str(enclosure.get("url", "")).lower().startswith("magnet:"):
                magnet = str(enclosure.get("url"))
            if not magnet and guid.lower().startswith("magnet:"):
                magnet = guid
            magnet_hash, trackers = g._parse_magnet(magnet) if magnet else (None, [])
            hash_value = attr("infohash") or attr("info_hash")
            hash_value = g.normalize_hash(hash_value) if hash_value else None
            guid_hash = guid.lower() if g.HASH_RE.fullmatch(guid) else None
            hashes = {x for x in (magnet_hash, hash_value, guid_hash) if x}
            if len(hashes) > 1:
                raise ValueError("magnet, GUID and infohash attributes disagree")
            ih = next(iter(hashes)) if hashes else None
            if not ih:
                raise ValueError("item has no verifiable infohash; download URLs are not fetched")
            if not title:
                raise ValueError("item title is empty")
            raw_categories = []
            for child in list(item):
                if _xml_name(child.tag) == "category" and child.text:
                    label = g._clean_text(child.text, "category", 256)
                    if label and label not in raw_categories:
                        raw_categories.append(label)
            category_ids = []
            for category_id in attrs.get("category", []):
                if category_id not in category_ids:
                    category_ids.append(category_id)
                label = category_map.get(category_id, category_id)
                if label and label not in raw_categories:
                    raw_categories.append(label)
            size_text = attr("size") or (str(enclosure.get("length", "")) if enclosure is not None else "")
            size = int(size_text) if size_text.isdigit() else None
            files_text = attr("files")
            files = int(files_text) if files_text.isdigit() else None
            result.append({
                "ih": ih, "title": title, "date": timestamp, "size": size, "files": files,
                "categories": raw_categories, "category_ids": category_ids, "trackers": trackers,
                "source_id": guid or ih,
                "provenance": {"guid": guid, "pubDate": published,
                               "seeders": attr("seeders"), "peers": attr("peers"),
                               "indexer": attr("indexer")},
            })
        except (ValueError, TypeError, OverflowError) as exc:
            result.append({"_error": str(exc), "_source_id": guid or "invalid-item",
                           "_published": timestamp, "_raw": ET.tostring(item, encoding="unicode")[:8192]})
        if len(result) > g.MAX_RECORDS:
            raise g.SourceError("RSS/Torznab response exceeds the item limit")
    return result


def torznab_source(source: Dict[str, Any], templates, state, run_id: str,
                   publisher: Dict[str, Any], quarantine_dir: Path) -> Dict[str, Any]:
    source_id = str(source["id"])
    endpoint = str(source.get("url", "")).strip()
    if not endpoint:
        return {"status": "skipped", "reason": "feed endpoint URL is not configured",
                "fetched": 0, "parsed": 0, "accepted": 0, "filtered": 0, "rejected": 0, "duplicates": 0}
    timeout = int(source.get("timeout", 30))
    page_size = int(source.get("page_size", 100))
    pages = int(source.get("max_pages", 5))
    max_items = int(source.get("max_items", 5000))
    overlap = int(source.get("overlap_seconds", 3 * 86400))
    if not 1 <= timeout <= 300 or not 1 <= page_size <= 1000 or not 1 <= pages <= 100:
        raise g.SourceError("feed timeout/page_size/max_pages is outside the supported range")
    if not 1 <= max_items <= 100000 or not 0 <= overlap <= 30 * 86400:
        raise g.SourceError("feed max_items/overlap_seconds is outside the supported range")
    api_key = str(source.get("api_key", ""))
    if len(api_key) > 512 or any(ord(ch) < 32 for ch in api_key):
        raise g.SourceError("feed API key is invalid")
    params = source.get("params", {}) or {}
    if not isinstance(params, dict) or any(str(k).casefold() in {"apikey", "api_key", "t", "limit", "offset"} for k in params):
        raise g.SourceError("feed params must not override API key, mode or pagination")
    previous = state.checkpoint(source_id) or {}
    is_rss = source.get("type") == "rss"
    caps_hash = previous.get("capabilities_sha256")
    category_map: Dict[str, str] = {}
    if not is_rss and source.get("fetch_caps", True):
        caps_data = _http_bytes(_torznab_url(endpoint, {"t": "caps"}, api_key), timeout, 8 * 1024 * 1024)
        category_map = _torznab_caps(caps_data)
        caps_hash = g._sha256(caps_data)
        for category_id, label in category_map.items():
            state.add_category(run_id, source_id, label, category_id)
    last_seen = int(previous.get("last_pubdate", 0))
    cutoff = max(0, last_seen - overlap) if last_seen else 0
    counters = {"status": "success", "fetched": 0, "parsed": 0, "accepted": 0,
                "filtered": 0, "rejected": 0, "duplicates": 0, "conflicts": 0,
                "quarantined": 0, "quarantine_truncated": False,
                "checkpoint": dict(previous), "errors": []}
    quarantine = quarantine_dir / f"{source_id}.jsonl"
    seen_guids = set()
    for page in range(1 if is_rss else pages):
        if is_rss:
            url = endpoint
        else:
            query = dict(params)
            query.update({"t": "search", "limit": page_size, "offset": page * page_size})
            url = _torznab_url(endpoint, query, api_key)
        data = _http_bytes(url, timeout)
        items = _rss_items(data, category_map)
        counters["fetched"] += len(items)
        for item in items:
            if counters["parsed"] + counters["rejected"] >= max_items:
                break
            item_id = str(item.get("source_id") or item.get("_source_id") or "unknown")[:256]
            source_ref = f"{source_id}#{item_id}"
            if item.get("_error"):
                counters["rejected"] += 1
                reason = str(item["_error"])
                if len(counters["errors"]) < g.MAX_ROW_ERRORS:
                    counters["errors"].append({"source_ref": source_ref, "error": reason})
                if g._quarantine(quarantine, source_id, source_ref, reason, item.get("_raw", "")):
                    counters["quarantined"] += 1
                else:
                    counters["quarantine_truncated"] = True
                continue
            timestamp = item.get("date")
            if timestamp is not None and cutoff and int(timestamp) < cutoff:
                counters["filtered"] += 1
                continue
            if item_id in seen_guids:
                counters["duplicates"] += 1
                continue
            seen_guids.add(item_id)
            try:
                categories = item["categories"]
                for category_id in item["category_ids"]:
                    state.add_category(run_id, source_id, category_map.get(category_id, category_id), category_id)
                for category in categories:
                    state.add_category(run_id, source_id, category)
                record = g._canonical_source_record({
                    "ih": item["ih"], "title": item["title"], "date": item["date"],
                    "size": item["size"], "files": item["files"],
                    "tags": g._tags_for(categories, publisher), "trackers": item["trackers"],
                }, publisher)
                outcome = state.add_observation(
                    run_id, source_id, record["ih"], int(source.get("priority", 40)),
                    record, categories, source_ref, str(timestamp or ""), item["provenance"],
                    deduplicate_persisted=True)
                counters["parsed"] += 1
                if outcome == "accepted":
                    counters["accepted"] += 1
                elif outcome == "duplicate":
                    counters["duplicates"] += 1
                elif outcome == "provenance":
                    counters["provenance_updated"] = counters.get("provenance_updated", 0) + 1
                else:
                    counters["conflicts"] += 1
                    reason = "conflicting duplicate infohash in feed"
                    if len(counters["errors"]) < g.MAX_ROW_ERRORS:
                        counters["errors"].append({"source_ref": source_ref, "error": reason})
                    if g._quarantine(quarantine, source_id, source_ref, reason, item["provenance"]):
                        counters["quarantined"] += 1
                    else:
                        counters["quarantine_truncated"] = True
                if timestamp is not None:
                    last_seen = max(last_seen, int(timestamp))
            except (ValueError, TypeError, OverflowError, KeyError) as exc:
                counters["rejected"] += 1
                if len(counters["errors"]) < g.MAX_ROW_ERRORS:
                    counters["errors"].append({"source_ref": source_ref, "error": str(exc)})
                if g._quarantine(quarantine, source_id, source_ref, str(exc), item):
                    counters["quarantined"] += 1
                else:
                    counters["quarantine_truncated"] = True
        if is_rss or len(items) < page_size or counters["parsed"] + counters["rejected"] >= max_items:
            break
    counters["checkpoint"] = {"kind": "rss" if is_rss else "torznab",
                              "last_pubdate": last_seen, "capabilities_sha256": caps_hash}
    return counters


def template_source(source: Dict[str, Any], templates, state, run_id: str,
                    publisher: Dict[str, Any], quarantine_dir: Path) -> Dict[str, Any]:
    source_id = str(source["id"])
    template_name = str(source.get("template", ""))
    template = templates.get(template_name)
    if not isinstance(template, dict):
        raise g.SourceError(f"template {template_name!r} is not defined in profile.templates")
    if source.get("path"):
        path = Path(str(source["path"])).expanduser().resolve()
        if not path.is_file() or path.stat().st_size > g.MAX_HTTP_BYTES:
            raise g.SourceError("template source file is missing or exceeds the size limit")
        payload = path.read_bytes()
        source_ref = str(path)
    elif source.get("url") and source.get("remote_enabled") is True:
        timeout = int(source.get("timeout", 30))
        if not 1 <= timeout <= 300:
            raise g.SourceError("template source timeout is outside the supported range")
        payload = _http_bytes(str(source["url"]), timeout, g.MAX_HTTP_BYTES)
        source_ref = "remote-template:" + template_name
    else:
        return {"status": "skipped", "reason": "template source path is not configured or remote access is disabled",
                "fetched": 0, "parsed": 0, "accepted": 0, "filtered": 0, "rejected": 0, "duplicates": 0}
    digest = g._sha256(payload)
    checkpoint = {"kind": "template", "template": template_name, "file_sha256": digest}
    previous = state.checkpoint(source_id)
    if previous and previous.get("file_sha256") == digest and previous.get("template") == template_name:
        return {"status": "unchanged", "checkpoint": checkpoint,
                "fetched": 0, "parsed": 0, "accepted": 0, "filtered": 0, "rejected": 0, "duplicates": 0}
    try:
        items = parse_template(payload, template, source_ref)
    except TemplateError as exc:
        raise g.SourceError(f"template {template_name!r}: {exc}") from exc
    counters = {"status": "success", "fetched": len(items), "parsed": 0, "accepted": 0,
                "filtered": 0, "rejected": 0, "duplicates": 0, "conflicts": 0,
                "quarantined": 0, "quarantine_truncated": False,
                "checkpoint": checkpoint, "errors": []}
    quarantine = quarantine_dir / f"{source_id}.jsonl"
    title_filter = g._compile_title_filter(source)
    for index, item in enumerate(items):
        source_ref = str(item.get("_source_ref") or f"{source_ref}#{index + 1}")
        try:
            if item.get("_error"):
                raise ValueError(str(item["_error"]))
            ih = item.get("ih")
            trackers = item.get("trackers") or []
            if isinstance(trackers, str):
                trackers = [trackers]
            if item.get("magnet"):
                magnet_hash, magnet_trackers = g._parse_magnet(item["magnet"])
                if ih and magnet_hash and g.normalize_hash(ih) != magnet_hash:
                    raise ValueError("template infohash and magnet disagree")
                ih = ih or magnet_hash
                trackers = list(trackers) + magnet_trackers
            title = g._clean_text(item.get("title"), "title")
            if g._title_filter_reason(title, title_filter):
                counters["filtered"] += 1
                continue
            categories = g._categories(item.get("categories"))
            if not categories and item.get("tags"):
                categories = g._categories(item.get("tags"))
                tags = categories
            else:
                tags = g._tags_for(categories, publisher)
            for category in categories:
                state.add_category(run_id, source_id, category)
            size = item.get("size")
            if size not in (None, ""):
                size = g._parse_size(size, str(source.get("size_unit", "binary")))
            raw = {"ih": ih, "title": title, "date": g._parse_date(item.get("date")),
                   "size": size, "files": int(item["files"]) if item.get("files") not in (None, "") else None,
                   "tags": tags, "trackers": trackers}
            record = g._canonical_source_record(raw, publisher)
            provenance = {key: item[key] for key in ("source_id", "description", "poster")
                          if item.get(key) not in (None, "")}
            outcome = state.add_observation(run_id, source_id, record["ih"],
                                            int(source.get("priority", 50)), record,
                                            categories, source_ref, digest, provenance)
            counters["parsed"] += 1
            if outcome == "accepted":
                counters["accepted"] += 1
            elif outcome == "duplicate":
                counters["duplicates"] += 1
            elif outcome == "provenance":
                counters["provenance_updated"] = counters.get("provenance_updated", 0) + 1
            else:
                counters["conflicts"] += 1
                reason = "conflicting duplicate infohash in template source"
                if g._quarantine(quarantine, source_id, source_ref, reason, item):
                    counters["quarantined"] += 1
                else:
                    counters["quarantine_truncated"] = True
        except (ValueError, TypeError, OverflowError, KeyError) as exc:
            counters["rejected"] += 1
            if len(counters["errors"]) < g.MAX_ROW_ERRORS:
                counters["errors"].append({"source_ref": source_ref, "error": str(exc)})
            if g._quarantine(quarantine, source_id, source_ref, str(exc), item):
                counters["quarantined"] += 1
            else:
                counters["quarantine_truncated"] = True
    return counters


def csv_source(source: Dict[str, Any], templates, state, run_id: str,
               publisher: Dict[str, Any], quarantine_dir: Path) -> Dict[str, Any]:
    """Register the existing CSV implementation behind the replaceable adapter API."""
    return g._csv_source(source, state, run_id, publisher, quarantine_dir)


def rutor_source(source: Dict[str, Any], templates, state, run_id: str,
                 publisher: Dict[str, Any], quarantine_dir: Path) -> Dict[str, Any]:
    """Register the existing Rutor implementation behind the replaceable adapter API."""
    return g._rutor_source(source, state, run_id, publisher, quarantine_dir)


SOURCE_ADAPTERS = {
    "csv": csv_source,
    "torrents-csv-data": csv_source,
    "rutor": rutor_source,
    "rutracker": rutracker_source,
    "torznab": torznab_source,
    "jackett-torznab": torznab_source,
    "rss": torznab_source,
    "template": template_source,
}
