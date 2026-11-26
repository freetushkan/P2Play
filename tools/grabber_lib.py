#!/usr/bin/env python3
"""Transactional, review-first catalog grabber (standard library only)."""
from __future__ import annotations

import base64
import csv
import datetime as dt
import decimal
import hashlib
import itertools
import json
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import zlib
from typing import Any, Dict, Iterator, List, Optional, Sequence, Tuple

MAX_SOURCE_BYTES = 64 * 1024 * 1024
MAX_LOCAL_CSV_BYTES = 16 * 1024 * 1024 * 1024
# Keep the independent legacy Rutor raw-DEFLATE expansion bound.
MAX_EXPANDED_BYTES = 512 * 1024 * 1024
MAX_RUTRACKER_COMPRESSED_BYTES = 16 * 1024 * 1024 * 1024
DEFAULT_RUTRACKER_COMPRESSED_BYTES = 8 * 1024 * 1024 * 1024
MAX_RUTRACKER_EXPANDED_BYTES = 128 * 1024 * 1024 * 1024
DEFAULT_RUTRACKER_EXPANDED_BYTES = 64 * 1024 * 1024 * 1024
MIN_RUTRACKER_XZ_MEMORY_BYTES = 16 * 1024 * 1024
MAX_RUTRACKER_XZ_MEMORY_BYTES = 1024 * 1024 * 1024
DEFAULT_RUTRACKER_XZ_MEMORY_BYTES = 128 * 1024 * 1024
DEFAULT_RUTRACKER_MAX_TOPICS = 10_000_000
MAX_RUTRACKER_TOPICS = 50_000_000
MAX_HTTP_BYTES = 64 * 1024 * 1024
MAX_JSON_DEPTH = 64
MAX_ROW_ERRORS = 200
MAX_QUARANTINE_BYTES = 2 * 1024 * 1024
MAX_CSV_FIELD_BYTES = 4 * 1024 * 1024
MAX_NDJSON_BYTES = 256 * 1024 * 1024
MAX_NDJSON_LINE = 64 * 1024
MAX_RECORDS = 1_000_000
HASH_RE = re.compile(r"^(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{64})$")
SHARD_RE = re.compile(r"^[a-z0-9_-]{1,64}$")
SIZE_RE = re.compile(r"^\s*([0-9]+(?:\.[0-9]+)?)\s*(B|KB|MB|GB|TB)\s*$", re.I)


class GrabberError(Exception):
    """Expected, actionable configuration or workflow error."""


class SourceError(Exception):
    """One source failed; other sources may continue."""


def _json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            chunk = stream.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def _nonempty(value: Any) -> bool:
    return value is not None and value != "" and value != [] and value != {}


def _clean_text(value: Any, field: str, limit: int = 2048) -> str:
    if value is None:
        return ""
    result = " ".join(str(value).replace("\x00", "").split())
    if len(result.encode("utf-8")) > limit:
        raise ValueError(f"{field} is too long")
    return result


def normalize_hash(value: Any) -> str:
    text = str(value or "").strip().lower()
    if not HASH_RE.fullmatch(text):
        raise ValueError("invalid 40- or 64-character infohash")
    return text


SENSITIVE_QUERY_KEYS = {"passkey", "key", "token", "auth", "authkey", "apikey", "api_key",
                        "password", "passwd", "secret", "session", "pass", "uid", "user_id"}


def _safe_tracker_url(value: Any) -> Optional[str]:
    text = str(value or "").strip()
    if not text or len(text) > 2048:
        return None
    try:
        parsed = urllib.parse.urlsplit(text)
        if parsed.scheme.lower() not in {"udp", "http", "https"} or not parsed.hostname:
            return None
        if parsed.username is not None or parsed.password is not None:
            return None
        # Accessing .port validates the syntax and the 0..65535 range.
        _ = parsed.port
        query = urllib.parse.parse_qsl(parsed.query, keep_blank_values=True)
        for key, _ in query:
            lowered = key.casefold().replace("-", "_")
            if lowered in SENSITIVE_QUERY_KEYS or any(token in lowered for token in ("passkey", "apikey", "auth", "token", "secret")):
                return None
        path_segments = [segment for segment in parsed.path.split("/") if segment]
        for segment in path_segments:
            if len(segment) >= 24 and re.fullmatch(r"[A-Za-z0-9_-]+", segment):
                return None
        return text
    except (ValueError, UnicodeError):
        return None


def _parse_magnet(value: Any) -> Tuple[Optional[str], List[str]]:
    if not value:
        return None, []
    text = str(value).strip()
    parsed = urllib.parse.urlsplit(text)
    if parsed.scheme.lower() != "magnet":
        raise ValueError("magnet field is not a magnet URI")
    query = urllib.parse.parse_qs(parsed.query, keep_blank_values=True)
    found_hash = None
    for candidate in query.get("xt", []):
        prefix = "urn:btih:"
        if candidate.lower().startswith(prefix):
            token = candidate[len(prefix):]
            if re.fullmatch(r"[0-9a-fA-F]{40}", token):
                found_hash = token.lower()
            elif re.fullmatch(r"[A-Z2-7a-z2-7]{32}", token):
                try:
                    found_hash = base64.b32decode(token.upper()).hex()
                except Exception as exc:
                    raise ValueError("invalid base32 btih in magnet") from exc
            else:
                raise ValueError("invalid btih in magnet")
            break
    trackers: List[str] = []
    for tracker in query.get("tr", []):
        safe = _safe_tracker_url(tracker)
        if safe and safe not in trackers:
            trackers.append(safe)
    return found_hash, trackers


def _parse_date(value: Any, *, require_timezone: bool = False) -> Optional[int]:
    if value is None or value == "":
        return None
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        number = int(value)
        if number < 0 or number > 253402300799:
            raise ValueError("date is outside the supported Unix timestamp range")
        return number
    text = str(value).strip()
    if re.fullmatch(r"[0-9]{1,12}", text):
        number = int(text)
        if number < 0 or number > 253402300799:
            raise ValueError("date is outside the supported Unix timestamp range")
        return number
    iso = text.replace("Z", "+00:00")
    try:
        parsed = dt.datetime.fromisoformat(iso)
    except ValueError as exc:
        raise ValueError("date is not Unix seconds or ISO-8601") from exc
    if parsed.tzinfo is None:
        if require_timezone:
            raise ValueError("ISO-8601 date must include its UTC offset")
        parsed = parsed.replace(tzinfo=dt.timezone.utc)
    seconds = int(parsed.timestamp())
    if seconds < 0:
        raise ValueError("date must not be before the Unix epoch")
    return seconds


def _parse_size(value: Any, unit_mode: str = "binary") -> Optional[int]:
    if value is None or value == "":
        return None
    if isinstance(value, int) and not isinstance(value, bool):
        if value < 0:
            raise ValueError("size must not be negative")
        return value
    match = SIZE_RE.fullmatch(str(value))
    if not match:
        if str(value).strip().isdigit():
            return int(str(value).strip())
        raise ValueError("size must be bytes or a value with B/KB/MB/GB/TB unit")
    number = decimal.Decimal(match.group(1))
    unit = match.group(2).upper()
    base = decimal.Decimal(1024 if unit_mode == "binary" else 1000)
    power = {"B": 0, "KB": 1, "MB": 2, "GB": 3, "TB": 4}[unit]
    result = (number * (base ** power)).quantize(decimal.Decimal("1"), rounding=decimal.ROUND_HALF_UP)
    integer = int(result)
    if integer < 0 or integer > 2**63 - 1:
        raise ValueError("size is outside the supported range")
    return integer


def _categories(value: Any) -> List[str]:
    if value is None or value == "":
        return []
    values = value if isinstance(value, list) else [value]
    result: List[str] = []
    for item in values:
        if isinstance(item, dict):
            item = item.get("Name", item.get("name", item.get("Title", item.get("title", ""))))
        text = _clean_text(item, "category", 256)
        if text and text not in result:
            result.append(text)
    return result


def _tags_for(categories: Sequence[str], publisher: Dict[str, Any]) -> List[str]:
    mapping = publisher.get("category_map", {}) or {}
    if not isinstance(mapping, dict):
        raise GrabberError("publisher.category_map must be an object")
    tags: List[str] = []
    for category in categories:
        mapped = mapping.get(category, category)
        values = mapped if isinstance(mapped, list) else [mapped]
        for value in values:
            tag = _clean_text(value, "tag", 128)
            if tag and tag not in tags:
                tags.append(tag)
    return sorted(tags, key=lambda value: value.casefold())


def _canonical_source_record(raw: Dict[str, Any], publisher: Dict[str, Any]) -> Dict[str, Any]:
    ih = normalize_hash(raw.get("ih"))
    title = _clean_text(raw.get("title"), "title")
    if not title:
        raise ValueError("title is empty")
    record: Dict[str, Any] = {"ih": ih, "title": title}
    date = _parse_date(raw.get("date"))
    size = raw.get("size")
    files = raw.get("files")
    if date is not None:
        record["date"] = date
    if size is not None:
        if isinstance(size, bool) or not isinstance(size, int) or size < 0 or size > 2**63 - 1:
            raise ValueError("size must be a non-negative integer")
        record["size"] = size
    if files is not None:
        if isinstance(files, bool) or not isinstance(files, int) or files < 0 or files > 2**31 - 1:
            raise ValueError("files must be a non-negative integer")
        record["files"] = files
    tags = raw.get("tags") or []
    if not isinstance(tags, list):
        raise ValueError("tags must be an array")
    normalized_tags = sorted({_clean_text(tag, "tag", 128) for tag in tags if _clean_text(tag, "tag", 128)}, key=str.casefold)
    if len(normalized_tags) > 256:
        raise ValueError("a record may contain at most 256 tags")
    if normalized_tags:
        record["tags"] = normalized_tags
    trackers = raw.get("trackers") or []
    if not isinstance(trackers, list):
        raise ValueError("trackers must be an array")
    normalized_trackers: List[str] = []
    for tracker in trackers:
        text = _clean_text(tracker, "tracker", 2048)
        safe = _safe_tracker_url(text)
        if safe and safe not in normalized_trackers:
            normalized_trackers.append(safe)
            if len(normalized_trackers) > 128:
                raise ValueError("a record may contain at most 128 trackers")
    if normalized_trackers:
        record["trackers"] = sorted(normalized_trackers)
    return record


def _record_fingerprint(record: Dict[str, Any]) -> str:
    return _sha256(_json(record).encode("utf-8"))


def _valid_shard(value: Any) -> str:
    shard = str(value or "default").strip().lower()
    if not SHARD_RE.fullmatch(shard):
        raise GrabberError(f"invalid shard name: {shard!r}")
    return shard


def _shard_for(record: Dict[str, Any], publisher: Dict[str, Any], previous: Optional[Dict[str, Any]]) -> str:
    if previous and previous.get("shard"):
        return _valid_shard(previous["shard"])
    policy = publisher.get("shard_policy", {}) or {}
    default = _valid_shard(policy.get("default", "default"))
    routes = policy.get("category_map", {}) or {}
    if not isinstance(routes, dict):
        raise GrabberError("publisher.shard_policy.category_map must be an object")
    for tag in record.get("tags", []):
        if tag in routes:
            return _valid_shard(routes[tag])
    return default


def _compile_title_filter(source: Dict[str, Any]) -> Tuple[str, List[re.Pattern], List[re.Pattern]]:
    filters = source.get("filters", {}) or {}
    spec = filters.get("title_regex", {}) if isinstance(filters, dict) else {}
    if not spec:
        return "any", [], []
    if not isinstance(spec, dict):
        raise GrabberError("filters.title_regex must be an object")
    mode = spec.get("mode", "any")
    if mode not in {"any", "all"}:
        raise GrabberError("title_regex.mode must be any or all")
    include = spec.get("include", []) or []
    exclude = spec.get("exclude", []) or []
    if not isinstance(include, list) or not isinstance(exclude, list):
        raise GrabberError("title_regex include/exclude must be arrays")
    if len(include) > 64 or len(exclude) > 64:
        raise GrabberError("at most 64 title include and exclude patterns are allowed")
    patterns = [str(x) for x in include + exclude]
    for pattern in patterns:
        if len(pattern) > 512 or re.search(r"\([^)]*[+*][^)]*\)[+*{]", pattern):
            raise GrabberError("title regex is too complex or contains nested unbounded quantifiers")
    try:
        return mode, [re.compile(str(x)) for x in include], [re.compile(str(x)) for x in exclude]
    except re.error as exc:
        raise GrabberError(f"invalid title regex: {exc}") from exc


def _title_filter_reason(title: str, compiled: Tuple[str, List[re.Pattern], List[re.Pattern]]) -> Optional[str]:
    mode, include, exclude = compiled
    for index, pattern in enumerate(exclude):
        if pattern.search(title):
            return f"exclude[{index}]"
    if not include:
        return None
    matches = [bool(pattern.search(title)) for pattern in include]
    accepted = any(matches) if mode == "any" else all(matches)
    if not accepted:
        return "include:no-match" if mode == "any" else "include:missing-pattern"
    return None


def _title_allowed(title: str, compiled: Tuple[str, List[re.Pattern], List[re.Pattern]]) -> bool:
    return _title_filter_reason(title, compiled) is None


SCHEMA = """
PRAGMA foreign_keys=ON;
CREATE TABLE IF NOT EXISTS runs (
  id TEXT PRIMARY KEY,
  status TEXT NOT NULL,
  full_path TEXT,
  delta_path TEXT,
  report_path TEXT,
  full_sha256 TEXT,
  delta_sha256 TEXT,
  full_count INTEGER NOT NULL DEFAULT 0,
  delta_count INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS source_checkpoints (
  source_id TEXT PRIMARY KEY,
  checkpoint_json TEXT NOT NULL,
  updated_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS observations (
  source_id TEXT NOT NULL,
  ih TEXT NOT NULL,
  priority INTEGER NOT NULL,
  record_json TEXT NOT NULL,
  categories_json TEXT NOT NULL,
  source_ref TEXT NOT NULL,
  revision TEXT NOT NULL,
  provenance_json TEXT NOT NULL DEFAULT '{}',
  PRIMARY KEY(source_id, ih)
);
CREATE INDEX IF NOT EXISTS observations_ih ON observations(ih);
CREATE TABLE IF NOT EXISTS staged_records (
  ih TEXT PRIMARY KEY,
  record_json TEXT NOT NULL,
  fingerprint TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS suppressions (
  ih TEXT PRIMARY KEY,
  reason TEXT NOT NULL,
  updated_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS overrides (
  ih TEXT PRIMARY KEY,
  record_json TEXT NOT NULL,
  updated_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS run_observations (
  run_id TEXT NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
  source_id TEXT NOT NULL,
  ih TEXT NOT NULL,
  priority INTEGER NOT NULL,
  record_json TEXT NOT NULL,
  categories_json TEXT NOT NULL,
  source_ref TEXT NOT NULL,
  revision TEXT NOT NULL,
  provenance_json TEXT NOT NULL DEFAULT '{}',
  PRIMARY KEY(run_id, source_id, ih)
);
CREATE INDEX IF NOT EXISTS run_observations_ih ON run_observations(run_id,ih);
CREATE TABLE IF NOT EXISTS run_conflicts (
  run_id TEXT NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
  source_id TEXT NOT NULL,
  ih TEXT NOT NULL,
  detail TEXT NOT NULL,
  PRIMARY KEY(run_id, source_id, ih)
);
CREATE TABLE IF NOT EXISTS run_sources (
  run_id TEXT NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
  source_id TEXT NOT NULL,
  status TEXT NOT NULL,
  checkpoint_json TEXT,
  report_json TEXT NOT NULL,
  PRIMARY KEY(run_id, source_id)
);
CREATE TABLE IF NOT EXISTS run_full_records (
  run_id TEXT NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
  ih TEXT NOT NULL,
  record_json TEXT NOT NULL,
  fingerprint TEXT NOT NULL,
  PRIMARY KEY(run_id, ih)
);
CREATE TABLE IF NOT EXISTS run_categories (
  run_id TEXT NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
  source_id TEXT NOT NULL,
  category_key TEXT NOT NULL,
  label TEXT NOT NULL,
  PRIMARY KEY(run_id, source_id, category_key)
);
"""


class State:
    def __init__(self, path: Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self.path = path
        self.db = sqlite3.connect(str(path), timeout=30)
        self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("PRAGMA synchronous=FULL")
        self.db.executescript(SCHEMA)
        for table in ("observations", "run_observations"):
            columns = {row[1] for row in self.db.execute(f"PRAGMA table_info({table})")}
            if "provenance_json" not in columns:
                self.db.execute(f"ALTER TABLE {table} ADD COLUMN provenance_json TEXT NOT NULL DEFAULT '{{}}'")
        self.db.commit()

    def close(self) -> None:
        self.db.close()

    def begin_run(self, run_id: str) -> None:
        active = self.db.execute("SELECT id FROM runs WHERE status IN ('building','pending') LIMIT 1").fetchone()
        if active:
            raise GrabberError(f"unresolved run {active['id']} exists; acknowledge or discard it first")
        self.db.execute("INSERT INTO runs(id,status,created_at) VALUES(?,?,?)", (run_id, "building", int(time.time())))
        self.db.commit()

    def checkpoint(self, source_id: str) -> Optional[Dict[str, Any]]:
        row = self.db.execute("SELECT checkpoint_json FROM source_checkpoints WHERE source_id=?", (source_id,)).fetchone()
        return json.loads(row[0]) if row else None

    def set_source_result(self, run_id: str, source_id: str, status: str,
                          checkpoint: Optional[Dict[str, Any]], report: Dict[str, Any]) -> None:
        self.db.execute("INSERT OR REPLACE INTO run_sources VALUES(?,?,?,?,?)",
                        (run_id, source_id, status, _json(checkpoint) if checkpoint is not None else None, _json(report)))
        self.db.commit()

    def add_observation(self, run_id: str, source_id: str, ih: str, priority: int,
                        record: Dict[str, Any], categories: Sequence[str],
                        source_ref: str, revision: str, provenance: Optional[Dict[str, Any]] = None,
                        deduplicate_persisted: bool = False) -> str:
        conflict = self.db.execute("SELECT 1 FROM run_conflicts WHERE run_id=? AND source_id=? AND ih=?",
                                   (run_id, source_id, ih)).fetchone()
        if conflict:
            return "conflict"
        packed = _json(record)
        packed_provenance = _json(provenance or {})
        packed_categories = _json(list(categories))
        existing = self.db.execute("SELECT record_json,categories_json,provenance_json,source_ref FROM run_observations WHERE run_id=? AND source_id=? AND ih=?",
                                   (run_id, source_id, ih)).fetchone()
        if existing:
            if existing["record_json"] == packed and existing["categories_json"] == packed_categories:
                if existing["provenance_json"] == packed_provenance:
                    return "duplicate"
                self.db.execute("UPDATE run_observations SET provenance_json=?,source_ref=? WHERE run_id=? AND source_id=? AND ih=?",
                                (packed_provenance, source_ref, run_id, source_id, ih))
                return "provenance"
            self.db.execute("DELETE FROM run_observations WHERE run_id=? AND source_id=? AND ih=?", (run_id, source_id, ih))
            detail = f"conflicting rows for the same infohash at {existing['source_ref']} and {source_ref}"
            self.db.execute("INSERT INTO run_conflicts VALUES(?,?,?,?)",
                            (run_id, source_id, ih, detail))
            return "conflict"
        if deduplicate_persisted:
            cached = self.db.execute(
                "SELECT priority,record_json,categories_json,provenance_json FROM observations WHERE source_id=? AND ih=?",
                (source_id, ih)).fetchone()
            if cached and cached["record_json"] == packed and cached["categories_json"] == packed_categories:
                if cached["priority"] == priority and cached["provenance_json"] == packed_provenance:
                    return "duplicate"
                # Preserve changing local metrics/provenance and priority updates,
                # but do not count an unchanged overlap replay as a new observation.
                outcome = "provenance" if cached["priority"] == priority else "accepted"
                self.db.execute("INSERT INTO run_observations VALUES(?,?,?,?,?,?,?,?,?)",
                                (run_id, source_id, ih, priority, packed, packed_categories, source_ref, revision, packed_provenance))
                return outcome
        self.db.execute("INSERT INTO run_observations VALUES(?,?,?,?,?,?,?,?,?)",
                        (run_id, source_id, ih, priority, packed, packed_categories, source_ref, revision, packed_provenance))
        return "accepted"

    def add_category(self, run_id: str, source_id: str, category: str, category_id: Optional[str] = None) -> None:
        key = str(category_id) if category_id not in (None, "") else category.casefold()
        label = f"{category_id}\t{category}" if category_id not in (None, "") else category
        self.db.execute("INSERT OR IGNORE INTO run_categories VALUES(?,?,?,?)", (run_id, source_id, key, label))

    def known_hashes(self, run_id: str) -> Iterator[str]:
        query = """SELECT ih FROM observations
                   UNION SELECT ih FROM run_observations WHERE run_id=?
                   UNION SELECT ih FROM overrides
                   UNION SELECT ih FROM staged_records
                   ORDER BY ih"""
        for row in self.db.execute(query, (run_id,)):
            yield str(row[0])

    def observations_for(self, run_id: str, ih: str) -> List[sqlite3.Row]:
        query = """SELECT source_id,priority,record_json,categories_json,source_ref,revision,provenance_json
                   FROM observations o WHERE ih=? AND NOT EXISTS
                     (SELECT 1 FROM run_observations r WHERE r.run_id=? AND r.source_id=o.source_id AND r.ih=o.ih)
                   UNION ALL
                   SELECT source_id,priority,record_json,categories_json,source_ref,revision,provenance_json
                   FROM run_observations WHERE run_id=? AND ih=?
                   ORDER BY priority,source_id"""
        return list(self.db.execute(query, (ih, run_id, run_id, ih)))

    def iter_catalog_inputs(self, run_id: str):
        query = """WITH active_obs AS (
                     SELECT o.ih,o.source_id,o.priority,o.record_json,o.categories_json,o.source_ref,o.revision,o.provenance_json
                     FROM observations o WHERE NOT EXISTS
                       (SELECT 1 FROM run_observations r WHERE r.run_id=? AND r.source_id=o.source_id AND r.ih=o.ih)
                     UNION ALL
                     SELECT ih,source_id,priority,record_json,categories_json,source_ref,revision,provenance_json
                     FROM run_observations WHERE run_id=?
                   ), all_hashes AS (
                     SELECT ih FROM observations
                     UNION SELECT ih FROM run_observations WHERE run_id=?
                     UNION SELECT ih FROM overrides
                     UNION SELECT ih FROM staged_records
                   )
                   SELECT h.ih,a.source_id,a.priority,a.record_json,a.categories_json,a.source_ref,a.revision,a.provenance_json,
                          st.record_json AS staged_json,ov.record_json AS override_json,
                          CASE WHEN su.ih IS NULL THEN 0 ELSE 1 END AS suppressed
                   FROM all_hashes h LEFT JOIN active_obs a ON a.ih=h.ih
                   LEFT JOIN staged_records st ON st.ih=h.ih
                   LEFT JOIN overrides ov ON ov.ih=h.ih
                   LEFT JOIN suppressions su ON su.ih=h.ih
                   ORDER BY h.ih,a.priority,a.source_id"""
        cursor = self.db.execute(query, (run_id, run_id, run_id))
        for ih, grouped in itertools.groupby(cursor, key=lambda row: row["ih"]):
            rows = list(grouped)
            observations = [row for row in rows if row["source_id"] is not None]
            staged_json = next((row["staged_json"] for row in rows if row["staged_json"] is not None), None)
            override_json = next((row["override_json"] for row in rows if row["override_json"] is not None), None)
            yield (ih, observations,
                   json.loads(staged_json) if staged_json else None,
                   json.loads(override_json) if override_json else None,
                   bool(rows[0]["suppressed"]))

    def staged_record(self, ih: str) -> Optional[Dict[str, Any]]:
        row = self.db.execute("SELECT record_json FROM staged_records WHERE ih=?", (ih,)).fetchone()
        return json.loads(row[0]) if row else None

    def is_suppressed(self, ih: str) -> bool:
        return self.db.execute("SELECT 1 FROM suppressions WHERE ih=?", (ih,)).fetchone() is not None

    def override_record(self, ih: str) -> Optional[Dict[str, Any]]:
        row = self.db.execute("SELECT record_json FROM overrides WHERE ih=?", (ih,)).fetchone()
        return json.loads(row[0]) if row else None

    def add_full_record(self, run_id: str, record: Dict[str, Any]) -> None:
        packed = _json(record)
        self.db.execute("INSERT INTO run_full_records VALUES(?,?,?,?)",
                        (run_id, record["ih"], packed, _record_fingerprint(record)))

    def finalize(self, run_id: str, full_path: Path, delta_path: Path, report_path: Path,
                 full_sha: str, delta_sha: str, full_count: int, delta_count: int) -> None:
        self.db.execute("UPDATE runs SET status='pending',full_path=?,delta_path=?,report_path=?,full_sha256=?,delta_sha256=?,full_count=?,delta_count=? WHERE id=?",
                        (str(full_path.resolve()), str(delta_path.resolve()), str(report_path.resolve()),
                         full_sha, delta_sha, full_count, delta_count, run_id))
        self.db.commit()

    def source_reports(self, run_id: str) -> Dict[str, Any]:
        return {row["source_id"]: json.loads(row["report_json"])
                for row in self.db.execute("SELECT source_id,report_json FROM run_sources WHERE run_id=?", (run_id,))}

    def write_category_dump(self, run_id: str, directory: Path) -> List[str]:
        directory.mkdir(parents=True, exist_ok=True)
        created = []
        for source_id in (row[0] for row in self.db.execute("SELECT DISTINCT source_id FROM run_categories WHERE run_id=? ORDER BY source_id", (run_id,))):
            safe = re.sub(r"[^a-zA-Z0-9_-]+", "_", source_id).strip("_") or "source"
            path = directory / f"{safe}.txt"
            rows = [row[0] for row in self.db.execute("SELECT label FROM run_categories WHERE run_id=? AND source_id=? ORDER BY label COLLATE NOCASE", (run_id, source_id))]
            _atomic_write(path, ("\n".join(rows) + ("\n" if rows else "")).encode("utf-8"))
            created.append(str(path))
        return created


def _atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + f".{os.getpid()}.tmp")
    with temp.open("wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temp, path)


def _write_ndjson_line(stream, digest, record: Dict[str, Any]) -> None:
    data = (_json(record) + "\n").encode("utf-8")
    if len(data) > MAX_NDJSON_LINE:
        raise GrabberError("generated NDJSON record exceeds 64 KiB")
    stream.write(data)
    digest.update(data)


def _quarantine(quarantine_path: Path, source_id: str, source_ref: str,
                error: str, raw: Any) -> bool:
    quarantine_path.parent.mkdir(parents=True, exist_ok=True)
    current_size = quarantine_path.stat().st_size if quarantine_path.exists() else 0
    payload = _json({"source": source_id, "source_ref": source_ref,
                     "error": error, "raw": str(raw)[:8192]}) + "\n"
    encoded = payload.encode("utf-8")
    if current_size + len(encoded) > MAX_QUARANTINE_BYTES:
        return False
    with quarantine_path.open("ab") as stream:
        stream.write(encoded)
    return True


def _git_revision(repo: Path, data_file: str, file_path: Path) -> Dict[str, Any]:
    try:
        commit = subprocess.run(["git", "-C", str(repo), "rev-parse", "--verify", "HEAD"],
                                check=True, capture_output=True, text=True, timeout=15).stdout.strip()
        blob = subprocess.run(["git", "-C", str(repo), "rev-parse", "--verify", f"HEAD:{data_file}"],
                              check=True, capture_output=True, text=True, timeout=15).stdout.strip()
        worktree = subprocess.run(["git", "-C", str(repo), "hash-object", str(file_path)],
                                  check=True, capture_output=True, text=True, timeout=15).stdout.strip()
    except (OSError, subprocess.SubprocessError) as exc:
        raise SourceError("cannot read local Git revision for CSV source") from exc
    if not re.fullmatch(r"[0-9a-f]{40,64}", commit) or not re.fullmatch(r"[0-9a-f]{40,64}", blob):
        raise SourceError("Git returned an invalid commit or blob ID")
    return {"commit_oid": commit, "blob_oid": blob, "worktree_oid": worktree}


def _read_limited_url(url: str, timeout: int) -> bytes:
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme not in {"http", "https"} or not parsed.hostname:
        raise SourceError("remote source URL must use HTTP or HTTPS")
    request = urllib.request.Request(url, headers={"User-Agent": "P2Play-Grabber/1.0"})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            length = response.headers.get("Content-Length")
            if length and int(length) > MAX_HTTP_BYTES:
                raise SourceError("remote source exceeds the compressed size limit")
            data = response.read(MAX_HTTP_BYTES + 1)
    except (urllib.error.URLError, TimeoutError, OSError, ValueError) as exc:
        raise SourceError("remote source download failed") from exc
    if len(data) > MAX_HTTP_BYTES:
        raise SourceError("remote source exceeds the compressed size limit")
    return data


def _decompress_raw_deflate(data: bytes) -> bytes:
    decoder = zlib.decompressobj(-15)
    try:
        output = decoder.decompress(data, MAX_EXPANDED_BYTES + 1)
        if len(output) > MAX_EXPANDED_BYTES or decoder.unconsumed_tail:
            raise SourceError("Rutor payload exceeds the decompressed size limit")
        remaining = MAX_EXPANDED_BYTES + 1 - len(output)
        output += decoder.flush(max(1, remaining))
    except zlib.error as exc:
        raise SourceError("Rutor cache is not a valid raw-DEFLATE stream") from exc
    if len(output) > MAX_EXPANDED_BYTES:
        raise SourceError("Rutor payload exceeds the decompressed size limit")
    if not decoder.eof or decoder.unused_data:
        raise SourceError("Rutor raw-DEFLATE stream is truncated or has trailing data")
    return output


def _csv_source(source: Dict[str, Any], state: State, run_id: str, publisher: Dict[str, Any],
                quarantine_dir: Path) -> Dict[str, Any]:
    source_id = str(source["id"])
    repo_text = source.get("repo")
    if repo_text:
        repo = Path(repo_text).expanduser().resolve()
        data_file = str(source.get("data_file", "torrents.csv"))
        if Path(data_file).is_absolute() or ".." in Path(data_file).parts:
            raise SourceError("CSV data_file must be a relative path inside the local checkout")
        path = (repo / data_file).resolve()
        if repo not in path.parents:
            raise SourceError("CSV data_file escapes the configured local checkout")
        revision = _git_revision(repo, data_file, path)
        checkpoint = {**revision, "kind": "git-csv"}
        revision_key = revision["worktree_oid"]
    elif source.get("path"):
        path = Path(str(source["path"])).expanduser().resolve()
        checkpoint = {"kind": "file-csv", "file_sha256": _file_sha256(path) if path.is_file() else ""}
        revision_key = checkpoint["file_sha256"]
    else:
        return {"status": "skipped", "reason": "local CSV checkout/path is not configured",
                "fetched": 0, "parsed": 0, "accepted": 0, "filtered": 0, "rejected": 0, "duplicates": 0}
    if not path.is_file():
        raise SourceError("configured CSV file does not exist")
    previous = state.checkpoint(source_id)
    if previous and previous.get("worktree_oid", previous.get("file_sha256")) == revision_key:
        return {"status": "unchanged", "checkpoint": checkpoint,
                "fetched": 0, "parsed": 0, "accepted": 0, "filtered": 0, "rejected": 0, "duplicates": 0}
    if path.stat().st_size > MAX_LOCAL_CSV_BYTES:
        raise SourceError("CSV file exceeds the configured local source size limit")

    counters = {"status": "success", "fetched": 0, "parsed": 0, "accepted": 0,
                "filtered": 0, "rejected": 0, "duplicates": 0, "conflicts": 0,
                "filter_reasons": {}, "quarantined": 0, "quarantine_truncated": False,
                "checkpoint": checkpoint, "errors": []}
    compiled = _compile_title_filter(source)
    quarantine_path = quarantine_dir / f"{source_id}.jsonl"
    old_limit = csv.field_size_limit()
    csv.field_size_limit(MAX_CSV_FIELD_BYTES)
    try:
        with path.open("r", encoding="utf-8-sig", newline="") as stream:
            reader = csv.DictReader(stream)
            if not reader.fieldnames or not {"infohash", "name"}.issubset(set(reader.fieldnames)):
                raise SourceError("CSV requires infohash and name columns")
            for row in reader:
                counters["fetched"] += 1
                source_ref = f"{path}:{reader.line_num}"
                try:
                    title = _clean_text(row.get("name"), "title")
                    if not title:
                        raise ValueError("title is empty")
                    filter_reason = _title_filter_reason(title, compiled)
                    if filter_reason:
                        counters["filtered"] += 1
                        counters["filter_reasons"][filter_reason] = counters["filter_reasons"].get(filter_reason, 0) + 1
                        continue
                    date_field = str(source.get("date_field", "created_unix"))
                    if date_field not in {"created_unix", "published"}:
                        raise SourceError("CSV date_field must be created_unix or published")
                    raw = {"ih": normalize_hash(row.get("infohash")), "title": title,
                           "size": int(row["size_bytes"]) if str(row.get("size_bytes", "")).strip() else None,
                           "files": int(row["files"]) if str(row.get("files", "")).strip() else None,
                           "date": int(row[date_field]) if str(row.get(date_field, "")).strip() else None}
                    record = _canonical_source_record(raw, publisher)
                    provenance = {}
                    for metric in ("published", "scraped_date", "seeders", "leechers", "completed"):
                        value = str(row.get(metric, "")).strip()
                        if value and re.fullmatch(r"[0-9]{1,20}", value):
                            provenance[metric] = int(value)
                    counters["parsed"] += 1
                    result = state.add_observation(run_id, source_id, record["ih"], int(source.get("priority", 0)),
                                                   record, [], source_ref, revision_key, provenance)
                    if result == "accepted":
                        counters["accepted"] += 1
                    elif result == "duplicate":
                        counters["duplicates"] += 1
                    elif result == "provenance":
                        counters["provenance_updated"] = counters.get("provenance_updated", 0) + 1
                    else:
                        counters["conflicts"] += 1
                        if len(counters["errors"]) < MAX_ROW_ERRORS:
                            counters["errors"].append({"source_ref": source_ref, "error": "conflicting duplicate infohash"})
                        if _quarantine(quarantine_path, source_id, source_ref, "conflicting duplicate infohash", row):
                            counters["quarantined"] += 1
                        else:
                            counters["quarantine_truncated"] = True
                except (ValueError, TypeError, OverflowError, KeyError) as exc:
                    counters["rejected"] += 1
                    if len(counters["errors"]) < MAX_ROW_ERRORS:
                        counters["errors"].append({"source_ref": source_ref, "error": str(exc)})
                    if _quarantine(quarantine_path, source_id, source_ref, str(exc), row):
                        counters["quarantined"] += 1
                    else:
                        counters["quarantine_truncated"] = True
    except (OSError, UnicodeError, csv.Error) as exc:
        raise SourceError(f"CSV read failed: {type(exc).__name__}") from exc
    finally:
        csv.field_size_limit(old_limit)
    return counters


def _rutor_source(source: Dict[str, Any], state: State, run_id: str, publisher: Dict[str, Any],
                  quarantine_dir: Path) -> Dict[str, Any]:
    source_id = str(source["id"])
    if source.get("path"):
        path = Path(str(source["path"])).expanduser().resolve()
        if not path.is_file():
            raise SourceError("configured Rutor cache does not exist")
        if path.stat().st_size > MAX_SOURCE_BYTES:
            raise SourceError("Rutor compressed cache exceeds the size limit")
        compressed = path.read_bytes()
        source_ref = str(path)
    elif source.get("url") and source.get("remote_enabled") is True:
        timeout = int(source.get("timeout", 30))
        if timeout < 1 or timeout > 300:
            raise SourceError("remote timeout must be between 1 and 300 seconds")
        compressed = _read_limited_url(str(source["url"]), timeout)
        source_ref = "remote:rutor.ls"
    else:
        return {"status": "skipped", "reason": "local cache is not configured; remote refresh is not explicitly enabled",
                "fetched": 0, "parsed": 0, "accepted": 0, "filtered": 0, "rejected": 0, "duplicates": 0}
    digest = _sha256(compressed)
    checkpoint = {"kind": "rutor-raw-deflate", "file_sha256": digest}
    previous = state.checkpoint(source_id)
    if previous and previous.get("file_sha256") == digest:
        return {"status": "unchanged", "checkpoint": checkpoint,
                "fetched": 0, "parsed": 0, "accepted": 0, "filtered": 0, "rejected": 0, "duplicates": 0}
    expanded = _decompress_raw_deflate(compressed)
    try:
        payload = json.loads(expanded)
    except RecursionError as exc:
        raise SourceError("Rutor JSON nesting exceeds the parser safety limit") from exc
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise SourceError("Rutor decompressed payload is not valid UTF-8 JSON") from exc
    stack = [(payload, 1)]
    while stack:
        value, depth = stack.pop()
        if depth > MAX_JSON_DEPTH:
            raise SourceError("Rutor JSON nesting exceeds the safety limit")
        if isinstance(value, dict):
            stack.extend((child, depth + 1) for child in value.values() if isinstance(child, (dict, list)))
        elif isinstance(value, list):
            stack.extend((child, depth + 1) for child in value if isinstance(child, (dict, list)))
    if not isinstance(payload, list):
        raise SourceError("Rutor JSON root must be an array")
    if len(payload) > MAX_RECORDS:
        raise SourceError("Rutor cache exceeds the publisher record limit")

    counters = {"status": "success", "fetched": len(payload), "parsed": 0, "accepted": 0,
                "filtered": 0, "rejected": 0, "duplicates": 0, "conflicts": 0,
                "quarantined": 0, "quarantine_truncated": False,
                "checkpoint": checkpoint, "errors": []}
    quarantine_path = quarantine_dir / f"{source_id}.jsonl"
    size_mode = str(source.get("size_unit", "binary"))
    if size_mode not in {"binary", "decimal"}:
        raise SourceError("Rutor size_unit must be binary or decimal")
    for index, item in enumerate(payload):
        source_row = f"{source_ref}#{index + 1}"
        try:
            if not isinstance(item, dict):
                raise ValueError("Rutor item is not an object")
            hash_from_magnet, trackers = _parse_magnet(item.get("Magnet")) if item.get("Magnet") else (None, [])
            hash_from_field = normalize_hash(item.get("Hash")) if item.get("Hash") else None
            if hash_from_field and hash_from_magnet and hash_from_field != hash_from_magnet:
                raise ValueError("Hash and Magnet infohash disagree")
            ih = hash_from_field or hash_from_magnet
            if not ih:
                raise ValueError("neither Hash nor Magnet contains an infohash")
            categories = _categories(item.get("Categories"))
            for category in categories:
                state.add_category(run_id, source_id, category)
            title = _clean_text(item.get("Title"), "title")
            raw = {"ih": ih, "title": title,
                   "date": _parse_date(item.get("CreateDate"), require_timezone=True),
                   "size": _parse_size(item.get("Size"), size_mode),
                   "tags": _tags_for(categories, publisher), "trackers": trackers}
            record = _canonical_source_record(raw, publisher)
            provenance = {}
            for key in ("Name", "Names", "Year", "Link", "IMDBID", "KPID", "VideoQuality", "AudioQuality", "Seed", "Peer"):
                value = item.get(key)
                if value not in (None, "", [], {}):
                    provenance[key] = value
            # Keep only bounded JSON-safe provenance; it never enters the signed record.
            packed_provenance = _json(provenance).encode("utf-8")
            if len(packed_provenance) > 8192:
                provenance = {"provenance_truncated": True}
            counters["parsed"] += 1
            result = state.add_observation(run_id, source_id, record["ih"], int(source.get("priority", 0)),
                                           record, categories, source_row, digest, provenance)
            if result == "accepted":
                counters["accepted"] += 1
            elif result == "duplicate":
                counters["duplicates"] += 1
            elif result == "provenance":
                counters["provenance_updated"] = counters.get("provenance_updated", 0) + 1
            else:
                counters["conflicts"] += 1
                if len(counters["errors"]) < MAX_ROW_ERRORS:
                    counters["errors"].append({"source_ref": source_row, "error": "conflicting duplicate infohash"})
                if _quarantine(quarantine_path, source_id, source_row, "conflicting duplicate infohash", item):
                    counters["quarantined"] += 1
                else:
                    counters["quarantine_truncated"] = True
        except (ValueError, TypeError, OverflowError, KeyError) as exc:
            counters["rejected"] += 1
            if len(counters["errors"]) < MAX_ROW_ERRORS:
                counters["errors"].append({"source_ref": source_row, "error": str(exc)})
            if _quarantine(quarantine_path, source_id, source_row, str(exc), item):
                counters["quarantined"] += 1
            else:
                counters["quarantine_truncated"] = True
    return counters


def _merge_observations(rows: Sequence[sqlite3.Row], publisher: Dict[str, Any]) -> Tuple[Optional[Dict[str, Any]], Optional[str]]:
    if not rows:
        return None, None
    decoded = []
    ih = ""
    for row in rows:
        record = json.loads(row["record_json"])
        ih = ih or str(record.get("ih") or record.get("del") or "")
        if "del" not in record:
            categories = json.loads(row["categories_json"])
            if categories:
                mapped_tags = _tags_for(categories, publisher)
                if mapped_tags:
                    record["tags"] = mapped_tags
                else:
                    record.pop("tags", None)
        decoded.append((int(row["priority"]), record))
    priorities = sorted({priority for priority, _ in decoded})
    merged: Optional[Dict[str, Any]] = None
    fields = ("title", "date", "size", "files", "tags", "trackers")
    for priority in priorities:
        group = [record for current_priority, record in decoded if current_priority == priority]
        tombstones = [record for record in group if "del" in record]
        live = [record for record in group if "del" not in record]
        if tombstones and live:
            return None, f"same-priority live/tombstone conflict at priority {priority}"
        if tombstones:
            shards = {_valid_shard(record.get("shard")) for record in tombstones}
            if len(shards) != 1:
                return None, f"same-priority tombstone shard conflict at priority {priority}"
            merged = {"del": ih, "shard": next(iter(shards))}
            continue
        if not live:
            continue
        # A higher-priority live source may restore an item after a lower-priority
        # explicit tombstone; restoration starts from that source's own fields.
        if merged is None or "del" in merged:
            merged = {"ih": ih}
        for field in fields:
            values = {_json(record[field]): record[field] for record in live if _nonempty(record.get(field))}
            if len(values) > 1:
                return None, f"same-priority source conflict at priority {priority}: {field}"
            if values:
                merged[field] = next(iter(values.values()))
    return merged, None


def _load_config(path: Path) -> Dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as stream:
            config = json.load(stream)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise GrabberError(f"cannot read JSON profile: {path}") from exc
    if not isinstance(config, dict) or isinstance(config.get("version"), bool) or config.get("version") != 1:
        raise GrabberError("profile must be an object with integer version: 1")
    sources = config.get("sources", [])
    if not isinstance(sources, list):
        raise GrabberError("profile.sources must be an array")
    seen = set()
    for source in sources:
        if not isinstance(source, dict) or not source.get("id"):
            raise GrabberError("each source must be an object with an id")
        source_id = str(source["id"])
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}", source_id):
            raise GrabberError(f"invalid source id: {source_id!r}")
        if source_id in seen:
            raise GrabberError(f"duplicate source id: {source_id}")
        seen.add(source_id)
        if "enabled" in source and not isinstance(source["enabled"], bool):
            raise GrabberError(f"enabled must be a boolean for source {source_id}")
        if "remote_enabled" in source and not isinstance(source["remote_enabled"], bool):
            raise GrabberError(f"remote_enabled must be a boolean for source {source_id}")
        try:
            if isinstance(source.get("priority", 0), bool):
                raise ValueError("boolean priority")
            priority = int(source.get("priority", 0))
        except (TypeError, ValueError) as exc:
            raise GrabberError(f"invalid priority for source {source_id}") from exc
        if priority < 0 or priority > 1_000_000:
            raise GrabberError(f"priority out of range for source {source_id}")
        if str(source.get("type") or source_id) == "rutracker":
            limits = (
                ("max_compressed_bytes", DEFAULT_RUTRACKER_COMPRESSED_BYTES, 1,
                 MAX_RUTRACKER_COMPRESSED_BYTES),
                ("max_expanded_bytes", DEFAULT_RUTRACKER_EXPANDED_BYTES, 1,
                 MAX_RUTRACKER_EXPANDED_BYTES),
                ("xz_memory_limit", DEFAULT_RUTRACKER_XZ_MEMORY_BYTES,
                 MIN_RUTRACKER_XZ_MEMORY_BYTES, MAX_RUTRACKER_XZ_MEMORY_BYTES),
                ("max_topics", DEFAULT_RUTRACKER_MAX_TOPICS, 1, MAX_RUTRACKER_TOPICS),
            )
            for field, default, minimum, maximum in limits:
                value = source.get(field, default)
                if (isinstance(value, bool) or not isinstance(value, int) or
                        not minimum <= value <= maximum):
                    raise GrabberError(
                        f"{field} for source {source_id} must be an integer "
                        f"between {minimum} and {maximum}"
                    )
        if source.get("date_field", "created_unix") not in {"created_unix", "published"}:
            raise GrabberError(f"invalid CSV date_field for source {source_id}")
    templates = config.get("templates", {}) or {}
    if not isinstance(templates, dict):
        raise GrabberError("profile.templates must be an object keyed by template name")
    from grabber_templates import TemplateError, validate_template
    for template_name, template in templates.items():
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}", str(template_name)):
            raise GrabberError(f"invalid template name: {template_name!r}")
        try:
            validate_template(template, str(template_name))
        except TemplateError as exc:
            raise GrabberError(f"invalid template {template_name!r}: {exc}") from exc
    for source in sources:
        if str(source.get("type", "")) == "template":
            template_name = str(source.get("template", ""))
            if template_name not in templates:
                raise GrabberError(f"source {source['id']!r} references undefined template {template_name!r}")
    publisher = config.get("publisher", {}) or {}
    if not isinstance(publisher, dict):
        raise GrabberError("profile.publisher must be an object")
    category_map = publisher.get("category_map", {}) or {}
    if not isinstance(category_map, dict):
        raise GrabberError("publisher.category_map must be an object")
    for category, mapped in category_map.items():
        if not isinstance(category, str) or not category:
            raise GrabberError("category_map keys must be non-empty strings")
        values = mapped if isinstance(mapped, list) else [mapped]
        if not values or any(not isinstance(value, str) for value in values):
            raise GrabberError(f"category_map value for {category!r} must be a string or non-empty string array")
        for value in values:
            _clean_text(value, "category tag", 128)
    policy = publisher.get("shard_policy", {}) or {}
    if not isinstance(policy, dict):
        raise GrabberError("publisher.shard_policy must be an object")
    _valid_shard(policy.get("default", "default"))
    shard_routes = policy.get("category_map", {}) or {}
    if not isinstance(shard_routes, dict):
        raise GrabberError("publisher.shard_policy.category_map must be an object")
    for category, shard in shard_routes.items():
        if not isinstance(category, str) or not category:
            raise GrabberError("shard category keys must be non-empty strings")
        _valid_shard(shard)
    return config


def run_grabber(profile_path: Path, full_path: Path, delta_path: Path,
                report_path: Path, state_path: Path,
                dump_categories: Optional[Path] = None) -> Dict[str, Any]:
    config = _load_config(profile_path)
    publisher = config.get("publisher", {}) or {}
    sources = []
    for item in config.get("sources", []):
        source = dict(item)
        for key in ("repo", "path"):
            if source.get(key):
                source_path = Path(str(source[key])).expanduser()
                if not source_path.is_absolute():
                    source_path = profile_path.expanduser().resolve().parent / source_path
                source[key] = str(source_path.resolve())
        sources.append(source)
    full_path, delta_path, report_path, state_path = [p.expanduser().resolve() for p in (full_path, delta_path, report_path, state_path)]
    for path in (full_path, delta_path, report_path):
        path.parent.mkdir(parents=True, exist_ok=True)
    state = State(state_path)
    run_id = str(uuid.uuid4())
    quarantine_dir = report_path.parent / "quarantine" / run_id
    try:
        state.begin_run(run_id)
        report: Dict[str, Any] = {"version": 1, "run_id": run_id, "status": "running",
                                  "created_at": int(time.time()), "sources": {}, "merge_conflicts": [],
                                  "counts": {"full": 0, "delta": 0}}
        enabled = 0
        successful_or_unchanged = 0
        for source in sources:
            source_id = str(source["id"])
            result: Dict[str, Any]
            if source.get("enabled", True) is not True:
                result = {"status": "disabled", "reason": "source disabled in profile"}
                state.set_source_result(run_id, source_id, "disabled", None, result)
            else:
                enabled += 1
                source_type = str(source.get("type") or source_id)
                before = state.checkpoint(source_id)
                savepoint = "source_" + str(len(report["sources"]))
                state.db.execute(f"SAVEPOINT {savepoint}")
                try:
                    from grabber_adapters import SOURCE_ADAPTERS
                    adapter = SOURCE_ADAPTERS.get(source_type)
                    if adapter is None:
                        result = {"status": "unsupported", "reason": f"adapter {source_type!r} is not registered"}
                    else:
                        result = adapter(source, config.get("templates", {}), state,
                                         run_id, publisher, quarantine_dir)
                    state.db.execute(f"RELEASE SAVEPOINT {savepoint}")
                    result["checkpoint_before"] = before
                    result["priority"] = int(source.get("priority", 0))
                    if result.get("status") in {"success", "unchanged"}:
                        successful_or_unchanged += 1
                    cached = state.db.execute("SELECT COUNT(*) FROM observations WHERE source_id=?", (source_id,)).fetchone()[0]
                    if result.get("status") in {"failed", "skipped"} and cached:
                        result["stale_cache_used"] = True
                    checkpoint_candidate = result.get("checkpoint")
                    if result.get("quarantine_truncated"):
                        result["checkpoint_deferred"] = True
                        checkpoint_candidate = None
                    state.set_source_result(run_id, source_id, result["status"], checkpoint_candidate, result)
                except (SourceError, GrabberError, OSError, ValueError, TypeError) as exc:
                    state.db.execute(f"ROLLBACK TO SAVEPOINT {savepoint}")
                    state.db.execute(f"RELEASE SAVEPOINT {savepoint}")
                    cached = state.db.execute("SELECT COUNT(*) FROM observations WHERE source_id=?", (source_id,)).fetchone()[0]
                    result = {"status": "failed", "reason": str(exc), "checkpoint_before": before,
                              "priority": int(source.get("priority", 0)), "stale_cache_used": bool(cached),
                              "fetched": 0, "parsed": 0, "accepted": 0, "filtered": 0,
                              "rejected": 0, "duplicates": 0, "errors": []}
                    state.set_source_result(run_id, source_id, "failed", None, result)
            report["sources"][source_id] = result

        # Category inventory remains useful when a source file has not changed.
        for saved in state.db.execute("SELECT source_id,categories_json FROM observations").fetchall():
            for category in json.loads(saved["categories_json"]):
                state.add_category(run_id, saved["source_id"], category)
        for saved in state.db.execute("SELECT source_id,categories_json FROM run_observations WHERE run_id=?", (run_id,)).fetchall():
            for category in json.loads(saved["categories_json"]):
                state.add_category(run_id, saved["source_id"], category)
        prior_observations = state.db.execute("SELECT COUNT(*) FROM observations").fetchone()[0]
        prior_manual = state.db.execute("SELECT COUNT(*) FROM overrides").fetchone()[0]
        prior_staged = state.db.execute("SELECT COUNT(*) FROM staged_records").fetchone()[0]
        if enabled == 0 or (successful_or_unchanged == 0 and prior_observations + prior_manual + prior_staged == 0):
            report["status"] = "no-op"
            report["reason"] = "no enabled, configured and usable source; no checkpoints advanced"
            state.db.execute("DELETE FROM runs WHERE id=?", (run_id,))
            state.db.commit()
            _atomic_write(report_path, (_json(report) + "\n").encode("utf-8"))
            if dump_categories:
                dump_categories.mkdir(parents=True, exist_ok=True)
            return report

        if full_path == delta_path or full_path == report_path or delta_path == report_path:
            raise GrabberError("full, delta and report outputs must be different files")
        digest_full = hashlib.sha256()
        digest_delta = hashlib.sha256()
        full_count = 0
        delta_count = 0
        full_temp = full_path.with_name(full_path.name + f".{run_id}.tmp")
        delta_temp = delta_path.with_name(delta_path.name + f".{run_id}.tmp")
        with full_temp.open("wb") as full_stream, delta_temp.open("wb") as delta_stream:
            for ih, rows, previous, override, suppressed in state.iter_catalog_inputs(run_id):
                if suppressed:
                    continue
                merged, conflict = _merge_observations(rows, publisher)
                if conflict:
                    if len(report["merge_conflicts"]) < MAX_ROW_ERRORS:
                        report["merge_conflicts"].append({"ih": ih, "error": conflict,
                            "sources": [row["source_id"] for row in rows],
                            "source_refs": [row["source_ref"] for row in rows]})
                    if previous is None:
                        continue
                    merged = previous
                if merged is not None and "del" in merged and override is None:
                    if previous is not None:
                        tombstone = {"del": ih, "shard": _valid_shard(previous.get("shard", merged["shard"]))}
                        _write_ndjson_line(delta_stream, digest_delta, tombstone)
                        delta_count += 1
                    continue
                if merged is None:
                    merged = override or previous
                    if merged is None:
                        continue
                merged["shard"] = _shard_for(merged, publisher, previous)
                if override:
                    merged = override
                # Revalidate local overrides and ensure deterministic canonical output.
                merged = _validate_export_record(merged, allow_delete=False)
                state.add_full_record(run_id, merged)
                _write_ndjson_line(full_stream, digest_full, merged)
                full_count += 1
                if full_count > MAX_RECORDS:
                    raise GrabberError("full catalog exceeds the publisher record limit")
                if previous is None or _record_fingerprint(previous) != _record_fingerprint(merged):
                    _write_ndjson_line(delta_stream, digest_delta, merged)
                    delta_count += 1
            # Explicit manual suppressions are represented as shard-correct tombstones
            # in ordinary delta mode. Snapshot-source omission never creates a tombstone.
            for row in state.db.execute("SELECT s.ih,s.record_json FROM staged_records s JOIN suppressions x ON x.ih=s.ih ORDER BY s.ih"):
                ih = row["ih"]
                tombstone = {"del": ih, "shard": json.loads(row["record_json"]).get("shard", "default")}
                _write_ndjson_line(delta_stream, digest_delta, tombstone)
                delta_count += 1
            full_stream.flush(); os.fsync(full_stream.fileno())
            delta_stream.flush(); os.fsync(delta_stream.fileno())
        if full_temp.stat().st_size > MAX_NDJSON_BYTES or delta_temp.stat().st_size > MAX_NDJSON_BYTES:
            raise GrabberError("generated NDJSON exceeds the publisher 256 MiB import limit")
        os.replace(full_temp, full_path)
        os.replace(delta_temp, delta_path)
        with state.db:
            state.finalize(run_id, full_path, delta_path, report_path, digest_full.hexdigest(),
                           digest_delta.hexdigest(), full_count, delta_count)
        if dump_categories:
            report["category_files"] = state.write_category_dump(run_id, dump_categories.expanduser().resolve())
        report["status"] = "degraded" if report["merge_conflicts"] or any(
            v.get("status") in {"failed", "unsupported"} or v.get("rejected", 0) or
            v.get("conflicts", 0) or v.get("quarantine_truncated")
            for v in report["sources"].values()) else "complete"
        report["counts"] = {"full": full_count, "delta": delta_count}
        report["files"] = {"full": str(full_path), "full_sha256": digest_full.hexdigest(),
                            "delta": str(delta_path), "delta_sha256": digest_delta.hexdigest()}
        report["state_db"] = str(state_path)
        report["quarantine_dir"] = str(quarantine_dir)
        report["next_step"] = "review the full file; publish delta if unchanged, otherwise use publisher --replace"
        _atomic_write(report_path, (_json(report) + "\n").encode("utf-8"))
        manifest = {"version": 1, "run_id": run_id, "status": "pending-review",
                    "full_path": str(full_path), "full_sha256": digest_full.hexdigest(),
                    "delta_path": str(delta_path), "delta_sha256": digest_delta.hexdigest(),
                    "full_count": full_count, "delta_count": delta_count,
                    "report_path": str(report_path), "state_db": str(state_path),
                    "source_status": {key: value.get("status") for key, value in report["sources"].items()}}
        pending_path = full_path.parent / "pending-run.json"
        _atomic_write(pending_path, (_json(manifest) + "\n").encode("utf-8"))
        report["pending_manifest"] = str(pending_path)
        _atomic_write(report_path, (_json(report) + "\n").encode("utf-8"))
        return report
    except Exception:
        state.db.execute("DELETE FROM runs WHERE id=? AND status IN ('building','pending')", (run_id,))
        state.db.commit()
        for output in (locals().get("full_temp"), locals().get("delta_temp")):
            if output:
                try:
                    Path(output).unlink(missing_ok=True)
                except OSError:
                    pass
        pending_path = full_path.parent / "pending-run.json"
        if pending_path.is_file():
            try:
                manifest = json.loads(pending_path.read_text(encoding="utf-8"))
                if manifest.get("run_id") == run_id:
                    pending_path.unlink()
            except (OSError, json.JSONDecodeError):
                pass
        raise
    finally:
        state.close()


def _validate_export_record(raw: Dict[str, Any], allow_delete: bool = True) -> Dict[str, Any]:
    if not isinstance(raw, dict):
        raise GrabberError("NDJSON record must be an object")
    if "del" in raw:
        if not allow_delete or set(raw) != {"del", "shard"}:
            raise GrabberError("deletion record must contain only del and shard")
        return {"del": normalize_hash(raw["del"]), "shard": _valid_shard(raw["shard"])}
    allowed = {"ih", "title", "shard", "date", "size", "files", "tags", "trackers"}
    unknown = set(raw) - allowed
    if unknown:
        raise GrabberError(f"unknown canonical field(s): {', '.join(sorted(unknown))}")
    ih = normalize_hash(raw.get("ih"))
    title = _clean_text(raw.get("title"), "title")
    if not title:
        raise GrabberError("title is empty")
    record: Dict[str, Any] = {"ih": ih, "title": title, "shard": _valid_shard(raw.get("shard", "default"))}
    for field in ("date", "size", "files"):
        if field in raw:
            value = raw[field]
            if isinstance(value, bool) or not isinstance(value, int) or value < 0:
                raise GrabberError(f"{field} must be a non-negative integer")
            record[field] = value
    if "tags" in raw:
        if not isinstance(raw["tags"], list):
            raise GrabberError("tags must be an array")
        tags = sorted({_clean_text(x, "tag", 128) for x in raw["tags"] if _clean_text(x, "tag", 128)}, key=str.casefold)
        if len(tags) > 256:
            raise GrabberError("a record may contain at most 256 tags")
        if tags:
            record["tags"] = tags
    if "trackers" in raw:
        if not isinstance(raw["trackers"], list):
            raise GrabberError("trackers must be an array")
        trackers = []
        for value in raw["trackers"]:
            text = _clean_text(value, "tracker", 2048)
            safe = _safe_tracker_url(text)
            if safe and safe not in trackers:
                trackers.append(safe)
        if len(trackers) > 128:
            raise GrabberError("a record may contain at most 128 trackers")
        if trackers:
            record["trackers"] = sorted(trackers)
    return record


def _read_ndjson(path: Path) -> Iterator[Dict[str, Any]]:
    if not path.is_file():
        raise GrabberError(f"NDJSON file does not exist: {path}")
    if path.stat().st_size > MAX_NDJSON_BYTES:
        raise GrabberError("NDJSON file exceeds the publisher import size limit")
    count = 0
    with path.open("r", encoding="utf-8") as stream:
        for line_no, line in enumerate(stream, 1):
            if len(line.encode("utf-8")) > MAX_NDJSON_LINE:
                raise GrabberError(f"NDJSON line {line_no} exceeds 64 KiB")
            if not line.strip():
                continue
            try:
                value = json.loads(line)
            except json.JSONDecodeError as exc:
                raise GrabberError(f"invalid JSON at NDJSON line {line_no}") from exc
            yield _validate_export_record(value)
            count += 1
            if count > MAX_RECORDS:
                raise GrabberError("NDJSON file exceeds the record limit")


def _pending_run(state: State, run_id: str) -> sqlite3.Row:
    row = state.db.execute("SELECT * FROM runs WHERE id=? AND status='pending'", (run_id,)).fetchone()
    if not row:
        raise GrabberError(f"pending run not found: {run_id}")
    return row


def ack_run(state_path: Path, run_id: str, full_file: Path,
            accepted_count: int, mode: str = "auto") -> Dict[str, Any]:
    if accepted_count < 0 or mode not in {"auto", "delta", "replace"}:
        raise GrabberError("accepted-count must be non-negative and mode must be auto, delta or replace")
    full_file = full_file.expanduser().resolve()
    state = State(state_path.expanduser().resolve())
    try:
        run = _pending_run(state, run_id)
        expected_full = Path(run["full_path"]).resolve()
        if full_file != expected_full:
            raise GrabberError("--file must point to the full catalog file recorded for this run")
        current_full_sha = _file_sha256(full_file)
        unchanged = current_full_sha == run["full_sha256"]
        selected_mode = ("delta" if unchanged else "replace") if mode == "auto" else mode
        if selected_mode == "delta" and not unchanged:
            raise GrabberError("full catalog was edited; publish it with --replace instead of the delta")
        input_path = Path(run["delta_path"] if selected_mode == "delta" else run["full_path"]).resolve()
        if selected_mode == "delta" and _file_sha256(input_path) != run["delta_sha256"]:
            raise GrabberError("delta file was edited independently; restore it from the report or use a reviewed full replacement")
        temp_name = f"ack_{uuid.uuid4().hex}"
        state.db.execute(f"CREATE TEMP TABLE {temp_name}(ih TEXT PRIMARY KEY,record_json TEXT NOT NULL)")
        count = 0
        for record in _read_ndjson(input_path):
            if "del" in record:
                # Tombstones are legal only in the delta stream, never in a replacement.
                if selected_mode == "replace":
                    raise GrabberError("replacement catalog must contain live records only")
                ih = record["del"]
                if state.db.execute(f"SELECT 1 FROM {temp_name} WHERE ih=?", (ih,)).fetchone():
                    raise GrabberError(f"duplicate infohash in publication file: {ih}")
                state.db.execute(f"INSERT INTO {temp_name} VALUES(?,?)", (ih, _json(record)))
            else:
                ih = record["ih"]
                if state.db.execute(f"SELECT 1 FROM {temp_name} WHERE ih=?", (ih,)).fetchone():
                    raise GrabberError(f"duplicate infohash in publication file: {ih}")
                state.db.execute(f"INSERT INTO {temp_name} VALUES(?,?)", (ih, _json(record)))
            count += 1
        if count != accepted_count:
            raise GrabberError(f"publisher accepted-count {accepted_count} does not match file record count {count}")

        with state.db:
            if selected_mode == "replace":
                # Manual omissions become durable suppressions. Manual edits/additions
                # become overrides so the next source refresh does not undo the review.
                omitted = state.db.execute(
                    f"SELECT p.ih FROM run_full_records p WHERE p.run_id=? AND NOT EXISTS (SELECT 1 FROM {temp_name} a WHERE a.ih=p.ih)",
                    (run_id,))
                for omitted_row in omitted:
                    ih = omitted_row["ih"]
                    state.db.execute("INSERT OR REPLACE INTO suppressions VALUES(?,?,?)",
                                     (ih, "omitted from reviewed authoritative full catalog", int(time.time())))
                    state.db.execute("DELETE FROM overrides WHERE ih=?", (ih,))
                state.db.execute(f"DELETE FROM suppressions WHERE ih IN (SELECT ih FROM {temp_name})")
                for row in state.db.execute(f"SELECT ih,record_json FROM {temp_name}").fetchall():
                    baseline = state.db.execute("SELECT record_json FROM run_full_records WHERE run_id=? AND ih=?", (run_id, row["ih"])).fetchone()
                    if not baseline or baseline[0] != row["record_json"]:
                        state.db.execute("INSERT OR REPLACE INTO overrides VALUES(?,?,?)",
                                         (row["ih"], row["record_json"], int(time.time())))
                state.db.execute("DELETE FROM staged_records")
                for row in state.db.execute(f"SELECT ih,record_json FROM {temp_name}"):
                    state.db.execute("INSERT INTO staged_records VALUES(?,?,?)",
                                     (row["ih"], row["record_json"], _record_fingerprint(json.loads(row["record_json"]))))
            else:
                for row in state.db.execute(f"SELECT record_json FROM {temp_name}").fetchall():
                    record = json.loads(row[0])
                    if "del" in record:
                        state.db.execute("DELETE FROM staged_records WHERE ih=?", (record["del"],))
                    else:
                        state.db.execute("INSERT OR REPLACE INTO staged_records VALUES(?,?,?)",
                                         (record["ih"], _json(record), _record_fingerprint(record)))

            for row in state.db.execute("SELECT source_id,ih,priority,record_json,categories_json,source_ref,revision,provenance_json FROM run_observations WHERE run_id=?", (run_id,)).fetchall():
                state.db.execute("INSERT OR REPLACE INTO observations VALUES(?,?,?,?,?,?,?,?)",
                                 tuple(row))
            for row in state.db.execute("SELECT source_id,checkpoint_json,status,report_json FROM run_sources WHERE run_id=?", (run_id,)).fetchall():
                details = json.loads(row["report_json"])
                if row["status"] in {"success", "unchanged"} and "priority" in details:
                    state.db.execute("UPDATE observations SET priority=? WHERE source_id=?",
                                     (int(details["priority"]), row["source_id"]))
                if row["checkpoint_json"] is not None:
                    state.db.execute("INSERT OR REPLACE INTO source_checkpoints VALUES(?,?,?)",
                                     (row["source_id"], row["checkpoint_json"], int(time.time())))
            state.db.execute("UPDATE runs SET status='acknowledged' WHERE id=?", (run_id,))
            state.db.execute(f"DROP TABLE {temp_name}")
            state.db.execute("DELETE FROM run_observations WHERE run_id=?", (run_id,))
            state.db.execute("DELETE FROM run_conflicts WHERE run_id=?", (run_id,))
            state.db.execute("DELETE FROM run_sources WHERE run_id=?", (run_id,))
            state.db.execute("DELETE FROM run_full_records WHERE run_id=?", (run_id,))
            state.db.execute("DELETE FROM run_categories WHERE run_id=?", (run_id,))
        manifest_path = full_file.parent / "pending-run.json"
        if manifest_path.is_file():
            try:
                manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
                if manifest.get("run_id") == run_id:
                    manifest["status"] = "acknowledged"
                    manifest["mode"] = selected_mode
                    manifest["accepted_count"] = count
                    manifest["acknowledged_at"] = int(time.time())
                    _atomic_write(manifest_path, (_json(manifest) + "\n").encode("utf-8"))
            except (OSError, json.JSONDecodeError):
                pass
        return {"run_id": run_id, "status": "acknowledged", "mode": selected_mode,
                "accepted_count": count, "full_sha256": current_full_sha}
    finally:
        state.close()


def discard_run(state_path: Path, run_id: str) -> Dict[str, Any]:
    state = State(state_path.expanduser().resolve())
    try:
        row = state.db.execute("SELECT status,full_path FROM runs WHERE id=?", (run_id,)).fetchone()
        if not row or row["status"] != "pending":
            raise GrabberError(f"pending run not found: {run_id}")
        manifest_path = Path(row["full_path"]).parent / "pending-run.json"
        state.db.execute("DELETE FROM runs WHERE id=?", (run_id,))
        state.db.commit()
        if manifest_path.is_file():
            try:
                manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
                if manifest.get("run_id") == run_id:
                    manifest["status"] = "discarded"
                    _atomic_write(manifest_path, (_json(manifest) + "\n").encode("utf-8"))
            except (OSError, json.JSONDecodeError):
                pass
        return {"run_id": run_id, "status": "discarded", "checkpoints_advanced": False}
    finally:
        state.close()


def manual_state(state_path: Path, action: str, kind: str = "both", ih: Optional[str] = None) -> Dict[str, Any]:
    if action not in {"list", "clear"}:
        raise GrabberError("manual action must be list or clear")
    if kind not in {"suppression", "override", "both"}:
        raise GrabberError("manual kind must be suppression, override or both")
    if action == "clear" and not ih:
        raise GrabberError("manual clear requires an infohash")
    normalized = normalize_hash(ih) if ih else None
    state = State(state_path.expanduser().resolve())
    try:
        if action == "clear":
            pending = state.db.execute("SELECT id FROM runs WHERE status IN ('building','pending') LIMIT 1").fetchone()
            if pending:
                raise GrabberError(f"resolve pending run {pending['id']} before changing manual state")
        result: Dict[str, Any] = {"status": action}
        with state.db:
            if action == "clear":
                removed = {"suppressions": 0, "overrides": 0}
                if kind in {"suppression", "both"}:
                    cursor = state.db.execute("DELETE FROM suppressions WHERE ih=?", (normalized,))
                    removed["suppressions"] = cursor.rowcount
                if kind in {"override", "both"}:
                    cursor = state.db.execute("DELETE FROM overrides WHERE ih=?", (normalized,))
                    removed["overrides"] = cursor.rowcount
                result.update({"ih": normalized, "removed": removed})
            else:
                suppressions = []
                overrides = []
                if kind in {"suppression", "both"}:
                    suppressions = [dict(row) for row in state.db.execute("SELECT ih,reason,updated_at FROM suppressions ORDER BY ih")]
                if kind in {"override", "both"}:
                    overrides = [{"ih": row["ih"], "record": json.loads(row["record_json"]), "updated_at": row["updated_at"]}
                                 for row in state.db.execute("SELECT ih,record_json,updated_at FROM overrides ORDER BY ih")]
                result.update({"suppressions": suppressions, "overrides": overrides})
        return result
    finally:
        state.close()


def build_parser():
    import argparse
    parser = argparse.ArgumentParser(description="Review-first P2Play catalog grabber")
    sub = parser.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run", help="fetch local/explicitly enabled sources and export editable NDJSON")
    run.add_argument("--profile", required=True, type=Path)
    run.add_argument("--output", required=True, type=Path, help="full authoritative NDJSON path")
    run.add_argument("--delta", required=True, type=Path, help="derived delta NDJSON path")
    run.add_argument("--report", required=True, type=Path)
    run.add_argument("--state", type=Path, help="SQLite state file (default: grabber.sqlite3 next to report)")
    run.add_argument("--dump-categories", type=Path)
    ack = sub.add_parser("ack", help="acknowledge successful publisher staging")
    ack.add_argument("--state", required=True, type=Path)
    ack.add_argument("--run-id", required=True)
    ack.add_argument("--file", required=True, type=Path, help="the full catalog file produced by run")
    ack.add_argument("--accepted-count", required=True, type=int)
    ack.add_argument("--mode", choices=("auto", "delta", "replace"), default="auto")
    discard = sub.add_parser("discard", help="discard a pending review run without advancing checkpoints")
    discard.add_argument("--state", required=True, type=Path)
    discard.add_argument("--run-id", required=True)
    manual = sub.add_parser("manual", help="list or clear persistent manual review decisions")
    manual.add_argument("action", choices=("list", "clear"))
    manual.add_argument("--state", required=True, type=Path)
    manual.add_argument("--kind", choices=("suppression", "override", "both"), default="both")
    manual.add_argument("--ih")
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        if args.command == "run":
            state_path = args.state or (args.report.parent / "grabber.sqlite3")
            result = run_grabber(args.profile, args.output, args.delta, args.report, state_path, args.dump_categories)
        elif args.command == "ack":
            result = ack_run(args.state, args.run_id, args.file, args.accepted_count, args.mode)
        elif args.command == "discard":
            result = discard_run(args.state, args.run_id)
        elif args.command == "manual":
            result = manual_state(args.state, args.action, args.kind, args.ih)
        else:
            return 2
        print(_json(result))
        return 0
    except (GrabberError, OSError, sqlite3.Error) as exc:
        print(f"grabber: error: {exc}", file=__import__("sys").stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
