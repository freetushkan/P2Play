#!/usr/bin/env python3
"""Authenticated, standard-library-only client for the P2Play publishing API."""

from __future__ import annotations

import datetime as dt
import getpass
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Sequence, Tuple

MAX_IMPORT_BYTES = 256 * 1024 * 1024
MAX_LINE_BYTES = 64 * 1024
MAX_RECORDS = 1_000_000
MAX_DESCRIPTION_BYTES = 64 * 1024
MAX_POSTER_BYTES = 2 * 1024 * 1024
MAX_RESPONSE_BYTES = 4 * 1024 * 1024
DEFAULT_WHITELIST = {
    "ih", "title", "shard", "date", "size", "files", "tags", "trackers",
    "poster", "description_ref", "poster_ref", "del",
}
INPUT_FIELDS = DEFAULT_WHITELIST | {
    "name", "category", "description", "desc", "description_file", "poster_file",
    "ih2", "site", "src", "file_list", "source_id",
}
DROPPED_SOURCE_FIELDS = {"ih2", "site", "src", "file_list", "source_id"}
HASH_RE = re.compile(r"^(?:[0-9a-fA-F]{40}|[0-9a-fA-F]{64})$")
SHARD_RE = re.compile(r"^[a-z0-9_-]{1,64}$")
PACK_RE = re.compile(r"^p-[0-9a-f]{64}\.bin$")


class PublisherError(Exception):
    """Expected, user-actionable publisher error."""


@dataclass
class MediaUpload:
    field: str
    kind: str
    content_type: str
    data: bytes


@dataclass
class PreparedImport:
    records: List[Dict[str, Any]]
    uploads: List[List[MediaUpload]]
    duplicates: int = 0
    dropped_fields: int = 0


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        # Never forward the Bearer token through a redirect.
        return None


class P2PlayApi:
    def __init__(self, base_url: str, timeout: int = 60):
        parsed = urllib.parse.urlsplit(base_url)
        if parsed.scheme not in ("http", "https") or not parsed.hostname:
            raise PublisherError("server URL must be an absolute http:// or https:// URL")
        if parsed.query or parsed.fragment or parsed.path not in ("", "/"):
            raise PublisherError("server URL must be an origin, without a path, query or fragment")
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self.token: Optional[str] = None
        self.opener = urllib.request.build_opener(NoRedirect())

    @staticmethod
    def require_secure_transport(base_url: str, allow_insecure_http: bool) -> None:
        parsed = urllib.parse.urlsplit(base_url)
        if parsed.scheme == "https" or allow_insecure_http:
            return
        host = (parsed.hostname or "").lower()
        loopback = host == "localhost"
        try:
            loopback = loopback or ipaddress.ip_address(host).is_loopback
        except ValueError:
            pass
        if not loopback:
            raise PublisherError(
                "refusing to send credentials over remote plain HTTP; use HTTPS or "
                "--allow-insecure-http only on a trusted network"
            )

    def request(self, method: str, path: str, body: Optional[bytes] = None,
                content_type: Optional[str] = None, authenticated: bool = True) -> Any:
        headers = {"User-Agent": "P2Play-Publisher/0.1", "Accept": "application/json"}
        if content_type:
            headers["Content-Type"] = content_type
        if authenticated:
            if not self.token:
                raise PublisherError("not authenticated")
            headers["Authorization"] = "Bearer " + self.token
        req = urllib.request.Request(self.base_url + path, data=body, headers=headers, method=method)
        try:
            with self.opener.open(req, timeout=self.timeout) as response:
                raw = response.read(MAX_RESPONSE_BYTES + 1)
            if len(raw) > MAX_RESPONSE_BYTES:
                raise PublisherError("server response is too large")
            if not raw:
                return None
            try:
                return json.loads(raw.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                raise PublisherError("server returned invalid JSON for " + path) from exc
        except urllib.error.HTTPError as exc:
            raw = exc.read(MAX_RESPONSE_BYTES + 1)
            message = ""
            try:
                value = json.loads(raw.decode("utf-8"))
                if isinstance(value, dict):
                    message = str(value.get("error") or value.get("message") or "")
            except (UnicodeDecodeError, json.JSONDecodeError):
                message = raw.decode("utf-8", "replace")[:500]
            suffix = ": " + message if message else ""
            raise PublisherError(f"{method} {path}: HTTP {exc.code}{suffix}") from exc
        except urllib.error.URLError as exc:
            raise PublisherError("cannot reach P2Play server: " + str(exc.reason)) from exc
        except TimeoutError as exc:
            raise PublisherError(f"request timed out: {method} {path}") from exc

    def login(self, username: str, password: str) -> None:
        body = json.dumps({"username": username, "password": password}).encode("utf-8")
        value = self.request("POST", "/api/auth/login", body, "application/json", False)
        token = value.get("token") if isinstance(value, dict) else None
        if not isinstance(token, str) or not token:
            raise PublisherError("login response did not contain an API token")
        self.token = token

    def lists(self) -> List[Dict[str, Any]]:
        value = self.request("GET", "/api/lists")
        items = value.get("lists") if isinstance(value, dict) else None
        if not isinstance(items, list):
            raise PublisherError("server returned an invalid list collection")
        return [item for item in items if isinstance(item, dict)]

    def create_list(self, name: str, description: str) -> str:
        body = json.dumps({"name": name, "description": description}).encode("utf-8")
        value = self.request("POST", "/api/publish", body, "application/json")
        key = value.get("pubkey") if isinstance(value, dict) else None
        if not isinstance(key, str) or not re.fullmatch(r"[0-9a-f]{64}", key):
            raise PublisherError("server did not return a valid list public key")
        return key

    def upload_media(self, pubkey: str, item: MediaUpload) -> Dict[str, Any]:
        value = self.request(
            "POST", f"/api/publish/{pubkey}/media/{item.kind}", item.data, item.content_type
        )
        if not isinstance(value, dict):
            raise PublisherError(f"server returned an invalid {item.kind} media reference")
        return value

    def add_records(self, pubkey: str, records: Sequence[Dict[str, Any]]) -> Dict[str, Any]:
        body = "".join(
            json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n" for row in records
        ).encode("utf-8")
        value = self.request(
            "POST", f"/api/publish/{pubkey}/records", body, "application/x-ndjson"
        )
        if not isinstance(value, dict):
            raise PublisherError("server returned an invalid import response")
        return value

    def release(self, pubkey: str) -> Dict[str, Any]:
        value = self.request("POST", f"/api/publish/{pubkey}/release", b"{}", "application/json")
        if not isinstance(value, dict):
            raise PublisherError("server returned an invalid release response")
        return value

    def status(self, pubkey: str) -> Dict[str, Any]:
        value = self.request("GET", f"/api/publish/{pubkey}/status")
        if not isinstance(value, dict):
            raise PublisherError("server returned an invalid publisher status")
        return value


def _load_object(path: Optional[str], label: str) -> Dict[str, Any]:
    if not path:
        return {}
    try:
        value = json.loads(Path(path).read_text(encoding="utf-8"))
    except OSError as exc:
        raise PublisherError(f"cannot read {label} {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise PublisherError(f"invalid JSON in {label} {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise PublisherError(label + " must contain a JSON object")
    return value


def load_whitelist(path: Optional[str]) -> set:
    if not path:
        return set(DEFAULT_WHITELIST)
    try:
        value = json.loads(Path(path).read_text(encoding="utf-8"))
    except OSError as exc:
        raise PublisherError(f"cannot read field whitelist {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise PublisherError(f"invalid JSON in field whitelist {path}: {exc}") from exc
    if not isinstance(value, list) or not all(isinstance(x, str) for x in value):
        raise PublisherError("field whitelist must be a JSON array of field names")
    allowed = set(value)
    unsupported = allowed - DEFAULT_WHITELIST
    if unsupported:
        raise PublisherError("unsupported whitelist fields: " + ", ".join(sorted(unsupported)))
    if not {"ih", "title", "del"}.issubset(allowed):
        raise PublisherError("field whitelist must retain ih, title and del")
    return allowed


def _timestamp(value: Any, line: int) -> int:
    if isinstance(value, bool):
        raise PublisherError(f"line {line}: date must be a Unix timestamp or ISO-8601 string")
    if isinstance(value, int):
        if value < 0 or value > (1 << 63) - 1:
            raise PublisherError(f"line {line}: date must be in the signed 64-bit Unix timestamp range")
        return value
    if isinstance(value, str):
        try:
            normalized = value[:-1] + "+00:00" if value.endswith("Z") else value
            parsed = dt.datetime.fromisoformat(normalized)
            if parsed.tzinfo is None:
                parsed = parsed.replace(tzinfo=dt.timezone.utc)
            result = int(parsed.timestamp())
            if result < 0 or result > (1 << 63) - 1:
                raise ValueError("date outside signed 64-bit Unix timestamp range")
            return result
        except ValueError as exc:
            raise PublisherError(f"line {line}: invalid ISO-8601 date {value!r}") from exc
    raise PublisherError(f"line {line}: date must be a Unix timestamp or ISO-8601 string")


def _uint(value: Any, field: str, line: int, maximum: int = (1 << 63) - 1) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0 or value > maximum:
        raise PublisherError(f"line {line}: {field} must be an integer from 0 to {maximum}")
    return value


def _strings(value: Any, field: str, line: int) -> List[str]:
    values = [value] if isinstance(value, str) else value
    if not isinstance(values, list) or len(values) > 64:
        raise PublisherError(f"line {line}: {field} must be a string or an array of at most 64 strings")
    out: List[str] = []
    seen = set()
    for item in values:
        if not isinstance(item, str) or len(item.encode("utf-8")) > 512:
            raise PublisherError(f"line {line}: each {field} item must be a string up to 512 bytes")
        item = item.strip()
        if item and item not in seen:
            out.append(item)
            seen.add(item)
    return out


def _categories(value: Any, mapping: Dict[str, Any], line: int) -> List[str]:
    if value is None:
        return []
    result: List[str] = []
    for category in _strings(value, "category", line):
        mapped = mapping.get(category.casefold(), category)
        if isinstance(mapped, str):
            result.append(mapped)
        elif isinstance(mapped, list) and all(isinstance(x, str) for x in mapped):
            result.extend(mapped)
        else:
            raise PublisherError(f"line {line}: category map value for {category!r} must be a string or string array")
    return _strings(result, "mapped tags", line)


def _local_file(value: Any, base_dir: Path, field: str, limit: int, line: int) -> bytes:
    if not isinstance(value, str) or not value:
        raise PublisherError(f"line {line}: {field} must be a local file path")
    path = Path(value)
    if not path.is_absolute():
        path = base_dir / path
    try:
        if path.stat().st_size > limit:
            raise PublisherError(f"line {line}: {field} exceeds {limit} bytes")
        data = path.read_bytes()
    except OSError as exc:
        raise PublisherError(f"line {line}: cannot read {field} {path}: {exc}") from exc
    if not data or len(data) > limit:
        raise PublisherError(f"line {line}: {field} is empty or too large")
    return data


def _image_type(data: bytes, line: int) -> str:
    if data.startswith(b"\xff\xd8\xff"):
        return "image/jpeg"
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        return "image/png"
    if len(data) >= 12 and data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        return "image/webp"
    raise PublisherError(f"line {line}: poster file is not JPEG, PNG or WebP")


def _media_ref(value: Any, kind: str, line: int) -> Dict[str, Any]:
    if not isinstance(value, dict):
        raise PublisherError(f"line {line}: {kind}_ref must be an object")
    pack, sha, media_type = value.get("pack"), value.get("sha256"), value.get("type")
    offset, length = value.get("offset"), value.get("length")
    type_ok = media_type == "text/plain; charset=utf-8" if kind == "description" else media_type in (
        "image/jpeg", "image/png", "image/webp"
    )
    max_length = MAX_DESCRIPTION_BYTES if kind == "description" else MAX_POSTER_BYTES
    if (not isinstance(pack, str) or not PACK_RE.fullmatch(pack) or
        not isinstance(sha, str) or not re.fullmatch(r"[0-9a-f]{64}", sha) or not type_ok or
        isinstance(offset, bool) or not isinstance(offset, int) or offset < 0 or
        isinstance(length, bool) or not isinstance(length, int) or not 0 < length <= max_length or
        offset + length > 16 * 1024 * 1024):
        raise PublisherError(f"line {line}: invalid {kind}_ref")
    return {"pack": pack, "sha256": sha, "offset": offset, "length": length, "type": media_type}


def normalize_row(raw: Any, line: int, base_dir: Path, whitelist: set,
                  category_map: Dict[str, Any], default_shard: str) -> Tuple[Dict[str, Any], List[MediaUpload], int]:
    if not isinstance(raw, dict):
        raise PublisherError(f"line {line}: expected a JSON object")
    unknown = set(raw) - INPUT_FIELDS
    if unknown:
        raise PublisherError(f"line {line}: unknown fields: " + ", ".join(sorted(unknown)))
    dropped = len(DROPPED_SOURCE_FIELDS.intersection(raw))
    for key in DROPPED_SOURCE_FIELDS:
        raw.pop(key, None)

    shard = raw.get("shard", default_shard)
    if not isinstance(shard, str) or not SHARD_RE.fullmatch(shard):
        raise PublisherError(f"line {line}: invalid shard name")
    row: Dict[str, Any] = {}
    uploads: List[MediaUpload] = []

    if "del" in raw:
        value = raw["del"]
        if not isinstance(value, str) or not HASH_RE.fullmatch(value):
            raise PublisherError(f"line {line}: del must be a 40- or 64-character hexadecimal hash")
        row.update({"del": value.lower(), "shard": shard})
        if "date" in raw:
            row["date"] = _timestamp(raw["date"], line)
    else:
        ih = raw.get("ih")
        title = raw.get("title", raw.get("name"))
        if "title" in raw and "name" in raw and raw["title"] != raw["name"]:
            raise PublisherError(f"line {line}: title and name disagree")
        if not isinstance(ih, str) or not HASH_RE.fullmatch(ih):
            raise PublisherError(f"line {line}: ih must be a 40- or 64-character hexadecimal hash")
        if not isinstance(title, str) or not title.strip() or "\x00" in title:
            raise PublisherError(f"line {line}: title/name must be non-empty UTF-8 text without NUL")
        try:
            title_bytes = title.encode("utf-8")
        except UnicodeEncodeError as exc:
            raise PublisherError(f"line {line}: title/name is not valid UTF-8 text") from exc
        if len(title_bytes) > 512:
            raise PublisherError(f"line {line}: title/name must be at most 512 UTF-8 bytes")
        row.update({"ih": ih.lower(), "title": title.strip(), "shard": shard})
        if "date" in raw:
            row["date"] = _timestamp(raw["date"], line)
        if "size" in raw:
            row["size"] = _uint(raw["size"], "size", line)
        if "files" in raw:
            row["files"] = _uint(raw["files"], "files", line, (1 << 31) - 1)
        tags = _strings(raw.get("tags", []), "tags", line)
        tags.extend(_categories(raw.get("category"), category_map, line))
        row["tags"] = _strings(tags, "tags", line) if tags else []
        if "trackers" in raw:
            row["trackers"] = _strings(raw["trackers"], "trackers", line)
        if "poster" in raw:
            if not isinstance(raw["poster"], str) or len(raw["poster"].encode("utf-8")) > 2048:
                raise PublisherError(f"line {line}: poster URL must be at most 2048 bytes")
            if raw["poster"]:
                row["poster"] = raw["poster"]
        for kind in ("description", "poster"):
            field = kind + "_ref"
            if field in raw:
                row[field] = _media_ref(raw[field], kind, line)

        description_fields = [key for key in ("description", "desc", "description_file") if key in raw]
        if len(description_fields) > 1:
            raise PublisherError(f"line {line}: use only one of description, desc or description_file")
        if description_fields and "description_ref" not in row:
            if "description_ref" in whitelist:
                key = description_fields[0]
                if key == "description_file":
                    data = _local_file(raw[key], base_dir, key, MAX_DESCRIPTION_BYTES, line)
                else:
                    if not isinstance(raw[key], str):
                        raise PublisherError(f"line {line}: {key} must be UTF-8 text")
                    data = raw[key].encode("utf-8")
                if not data or len(data) > MAX_DESCRIPTION_BYTES or b"\x00" in data:
                    raise PublisherError(f"line {line}: description is empty, contains NUL or exceeds 64 KiB")
                try:
                    data.decode("utf-8")
                except UnicodeDecodeError as exc:
                    raise PublisherError(f"line {line}: description is not valid UTF-8") from exc
                uploads.append(MediaUpload("description_ref", "description", "text/plain; charset=utf-8", data))
            else:
                dropped += 1
        if "poster_file" in raw and "poster_ref" not in row:
            if "poster_ref" in whitelist:
                data = _local_file(raw["poster_file"], base_dir, "poster_file", MAX_POSTER_BYTES, line)
                uploads.append(MediaUpload("poster_ref", "poster", _image_type(data, line), data))
            else:
                dropped += 1

    for key in list(row):
        if key not in whitelist:
            if key in ("ih", "title", "del"):
                raise PublisherError(f"line {line}: whitelist removes required field {key}")
            del row[key]
            dropped += 1
    uploads = [item for item in uploads if item.field in whitelist]
    if not ("del" in row or ("ih" in row and "title" in row)):
        raise PublisherError(f"line {line}: whitelist removed record identity")
    if len(json.dumps(row, ensure_ascii=False).encode("utf-8")) > MAX_LINE_BYTES:
        raise PublisherError(f"line {line}: normalized record exceeds 64 KiB")
    return row, uploads, dropped


def prepare_import(path: str, whitelist: set, category_map: Dict[str, Any],
                   default_shard: str) -> PreparedImport:
    source = Path(path)
    try:
        if source.stat().st_size > MAX_IMPORT_BYTES:
            raise PublisherError(f"input file exceeds {MAX_IMPORT_BYTES} bytes")
        content = source.read_bytes()
    except OSError as exc:
        raise PublisherError(f"cannot read input file {path}: {exc}") from exc
    try:
        text = content.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        raise PublisherError("input file must be UTF-8 encoded") from exc

    rows: List[Dict[str, Any]] = []
    uploads: List[List[MediaUpload]] = []
    seen: Dict[str, Tuple[str, int]] = {}
    duplicates = dropped_fields = 0
    for line_no, line_text in enumerate(text.splitlines(), 1):
        if not line_text.strip():
            continue
        if len(line_text.encode("utf-8")) > MAX_LINE_BYTES:
            raise PublisherError(f"line {line_no}: input line exceeds 64 KiB")
        try:
            raw = json.loads(line_text)
        except json.JSONDecodeError as exc:
            raise PublisherError(f"line {line_no}: invalid JSON: {exc.msg}") from exc
        row, media, dropped = normalize_row(
            raw, line_no, source.resolve().parent, whitelist, category_map, default_shard
        )
        dropped_fields += dropped
        identity = row.get("ih") or row.get("del")
        fingerprint_data = {
            "record": row,
            "media": [
                {"field": x.field, "kind": x.kind, "sha256": hashlib.sha256(x.data).hexdigest()}
                for x in media
            ],
        }
        fingerprint = json.dumps(fingerprint_data, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
        if identity in seen:
            prior, prior_line = seen[identity]
            if fingerprint != prior:
                raise PublisherError(
                    f"line {line_no}: conflicting records for {identity} (first on line {prior_line}); "
                    "resolve source priority before publishing"
                )
            duplicates += 1
            continue
        seen[identity] = (fingerprint, line_no)
        rows.append(row)
        uploads.append(media)
        if len(rows) > MAX_RECORDS:
            raise PublisherError(f"input exceeds {MAX_RECORDS} records")
    if not rows:
        raise PublisherError("input contains no publishable records")
    return PreparedImport(rows, uploads, duplicates, dropped_fields)


def _password(env_name: str) -> str:
    value = os.environ.get(env_name)
    if value:
        return value
    try:
        return getpass.getpass("P2Play password: ")
    except (EOFError, OSError) as exc:
        raise PublisherError("password not found in environment variable " + env_name +
                             " and no interactive terminal is available") from exc


def preview(prepared: PreparedImport, dry_run: bool) -> None:
    upload_count = sum(len(x) for x in prepared.uploads)
    upload_bytes = sum(len(media.data) for group in prepared.uploads for media in group)
    prefix = "Dry-run" if dry_run else "Validated"
    print(
        f"{prefix}: {len(prepared.records)} records, {prepared.duplicates} identical duplicates skipped, "
        f"{prepared.dropped_fields} known source/filtered fields omitted, "
        f"{upload_count} media uploads ({upload_bytes} bytes)"
    )


def publish_import(args: Any) -> int:
    if not SHARD_RE.fullmatch(args.shard):
        raise PublisherError("--shard must match [a-z0-9_-] and be 1..64 characters")
    whitelist = load_whitelist(args.field_whitelist)
    mapping = {str(k).casefold(): v for k, v in _load_object(args.category_map, "category map").items()}
    prepared = prepare_import(args.import_path, whitelist, mapping, args.shard)
    preview(prepared, args.dry_run)
    if args.dry_run:
        print("No network requests made.")
        return 0
    if bool(args.list) == bool(args.create_list):
        raise PublisherError("choose exactly one of --list PUBKEY or --create-list NAME")
    if args.create_list and (
        not args.create_list.strip() or len(args.create_list.encode("utf-8")) > 256
    ):
        raise PublisherError("--create-list name must be 1..256 UTF-8 bytes")

    P2PlayApi.require_secure_transport(args.server, args.allow_insecure_http)
    api = P2PlayApi(args.server, args.timeout)
    api.login(args.username, _password(args.password_env))
    if args.create_list:
        pubkey = api.create_list(args.create_list, args.description)
        print("Created catalog list: " + pubkey)
    else:
        if not isinstance(args.list, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", args.list):
            raise PublisherError("--list must be a 64-character hexadecimal public key")
        pubkey = args.list.lower()
        owned = next((x for x in api.lists() if x.get("pubkey") == pubkey), None)
        if not owned or owned.get("own") is not True:
            raise PublisherError("the selected list is not owned by this P2Play server")
        if owned.get("type", "catalog") != "catalog":
            raise PublisherError("meta-lists cannot receive catalog records")

    # Validate everything locally before the first write. A failed media POST may
    # leave staged orphan bytes; POST requests are never retried automatically.
    for row, items in zip(prepared.records, prepared.uploads):
        for item in items:
            row[item.field] = api.upload_media(pubkey, item)
    result = api.add_records(pubkey, prepared.records)
    if result.get("count") != len(prepared.records):
        raise PublisherError(
            "server accepted an unexpected record count; inspect publisher status before retrying"
        )
    print(f"Staged {result.get('count')} records in {pubkey}; pending: {result.get('pending')}")
    if args.release:
        released = api.release(pubkey)
        print(f"Released sequence {released.get('seq')} ({released.get('records')} live records)")
    else:
        status = api.status(pubkey)
        print(f"Release not requested; pending: {status.get('pending')}")
    print("The private Ed25519 key stayed on the P2Play server.")
    return 0


def build_parser():
    import argparse

    parser = argparse.ArgumentParser(prog="p2play", description="P2Play operator tools")
    commands = parser.add_subparsers(dest="command", required=True)
    publish = commands.add_parser("publish", help="import catalog records")
    publish.add_argument("--import", dest="import_path", required=True, metavar="NDJSON",
                         help="UTF-8 newline-delimited JSON input")
    publish.add_argument("--server", default="http://127.0.0.1:8090",
                         help="P2Play server origin")
    publish.add_argument("--username", default="owner", help="P2Play account (default: owner)")
    publish.add_argument("--password-env", default="P2PLAY_PASSWORD",
                         help="password environment variable (otherwise prompt)")
    target = publish.add_mutually_exclusive_group()
    target.add_argument("--list", help="existing owned catalog list public key")
    target.add_argument("--create-list", metavar="NAME", help="create a new catalog list")
    publish.add_argument("--description", default="", help="description for a new catalog list")
    publish.add_argument("--category-map", help="JSON object mapping category names to tag(s)")
    publish.add_argument("--field-whitelist", help="JSON array of canonical fields to retain")
    publish.add_argument("--shard", default="default", help="default shard for records")
    publish.add_argument("--release", action="store_true",
                         help="sign and publish the next version after import")
    publish.add_argument("--dry-run", action="store_true",
                         help="validate and summarize input without contacting the server")
    publish.add_argument("--allow-insecure-http", action="store_true",
                         help="allow credentials over non-loopback plain HTTP")
    publish.add_argument("--timeout", type=int, default=60, help="HTTP timeout in seconds")
    return parser
