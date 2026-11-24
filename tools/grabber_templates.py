#!/usr/bin/env python3
"""Data-driven, non-executable source parser templates for P2Play Grabber."""
from __future__ import annotations

from html.parser import HTMLParser
import json
import re
from typing import Any, Dict, List, Optional, Sequence, Tuple

MAX_TEMPLATE_BYTES = 16 * 1024 * 1024
MAX_TEMPLATE_ITEMS = 100_000
MAX_TEMPLATE_LINE = 16 * 1024
MAX_SELECTOR_LENGTH = 256
MAX_REGEX_LENGTH = 512
MAX_HTML_NODES = 250_000
FIELD_NAMES = {
    "ih", "magnet", "title", "date", "size", "files", "categories",
    "tags", "trackers", "source_id", "description", "poster",
}


class TemplateError(ValueError):
    """Invalid template or source payload."""


def _safe_pattern(pattern: Any) -> re.Pattern:
    if not isinstance(pattern, str) or not pattern or len(pattern) > MAX_REGEX_LENGTH:
        raise TemplateError("template regex must be a non-empty string of at most 512 characters")
    if re.search(r"\([^)]*[+*][^)]*\)[+*{]", pattern):
        raise TemplateError("template regex contains nested unbounded quantifiers")
    try:
        compiled = re.compile(pattern)
    except re.error as exc:
        raise TemplateError(f"invalid template regex: {exc}") from exc
    if compiled.groups > 64:
        raise TemplateError("template regex may contain at most 64 capture groups")
    return compiled


def _parse_json_path(path: str) -> List[Tuple[str, Any]]:
    if not isinstance(path, str) or not path or len(path) > 256:
        raise TemplateError("JSON path must be a non-empty string of at most 256 characters")
    if path.startswith("$"):
        path = path[1:]
    tokens: List[Tuple[str, Any]] = []
    index = 0
    while index < len(path):
        if path[index] == ".":
            match = re.match(r"\.([A-Za-z_][A-Za-z0-9_-]*)", path[index:])
            if not match:
                raise TemplateError(f"invalid JSON path near {path[index:]!r}")
            tokens.append(("key", match.group(1)))
            index += len(match.group(0))
        elif path[index] == "[":
            end = path.find("]", index + 1)
            if end < 0:
                raise TemplateError("unclosed JSON path bracket")
            value = path[index + 1:end]
            if value in {"*", ""}:
                tokens.append(("wildcard", None))
            elif value.isdigit():
                tokens.append(("index", int(value)))
            elif re.fullmatch(r"""['"][A-Za-z_][A-Za-z0-9_-]*['"]""", value):
                tokens.append(("key", value[1:-1]))
            else:
                raise TemplateError(f"unsupported JSON path bracket {value!r}")
            index = end + 1
        else:
            raise TemplateError(f"invalid JSON path near {path[index:]!r}")
    return tokens


def _json_path_values(root: Any, path: str) -> List[Any]:
    current = [root]
    for operation, argument in _parse_json_path(path):
        next_values: List[Any] = []
        for value in current:
            if operation == "key":
                if isinstance(value, dict) and argument in value:
                    next_values.append(value[argument])
            elif operation == "index":
                if isinstance(value, list) and argument < len(value):
                    next_values.append(value[argument])
            elif operation == "wildcard":
                if isinstance(value, list):
                    next_values.extend(value)
                elif isinstance(value, dict):
                    next_values.extend(value[key] for key in sorted(value))
        current = next_values
        if len(current) > MAX_TEMPLATE_ITEMS:
            raise TemplateError("JSON path expands to too many values")
    return current


def _field_spec(value: Any, fmt: str) -> Dict[str, Any]:
    if isinstance(value, str):
        key = "path" if fmt == "json" else "selector" if fmt == "html" else "group"
        return {key: value}
    if not isinstance(value, dict):
        raise TemplateError("each template field must be a string or object")
    return dict(value)


def validate_template(template: Any, name: str = "template") -> Dict[str, Any]:
    if not isinstance(template, dict):
        raise TemplateError(f"{name} must be an object")
    fmt = template.get("format")
    if fmt not in {"json", "html", "text"}:
        raise TemplateError(f"{name}.format must be json, html or text")
    fields = template.get("fields", {})
    if not isinstance(fields, dict) or not fields:
        raise TemplateError(f"{name}.fields must be a non-empty object")
    unknown = set(fields) - FIELD_NAMES
    if unknown:
        raise TemplateError(f"{name} has unsupported field(s): {', '.join(sorted(unknown))}")
    for field, raw_spec in fields.items():
        spec = _field_spec(raw_spec, fmt)
        if fmt == "json":
            if "path" not in spec:
                raise TemplateError(f"{name}.fields.{field} requires path")
            _parse_json_path(spec["path"])
        elif fmt == "html":
            selector = spec.get("selector")
            if not isinstance(selector, str) or len(selector) > MAX_SELECTOR_LENGTH:
                raise TemplateError(f"{name}.fields.{field}.selector is invalid")
            _parse_selector(selector)
            attr = spec.get("attr")
            if attr is not None and not re.fullmatch(r"[A-Za-z_:][-A-Za-z0-9_:.]*", str(attr)):
                raise TemplateError(f"{name}.fields.{field}.attr is invalid")
        else:
            group = spec.get("group", field)
            if not isinstance(group, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", group):
                raise TemplateError(f"{name}.fields.{field}.group is invalid")
    if fmt == "json":
        record_path = template.get("record_path", "$[*]")
        _parse_json_path(record_path)
    elif fmt == "html":
        selector = template.get("item_selector")
        if not isinstance(selector, str) or len(selector) > MAX_SELECTOR_LENGTH:
            raise TemplateError(f"{name}.item_selector is required and must be a simple CSS-like selector")
        _parse_selector(selector)
    else:
        _safe_pattern(template.get("pattern"))
    return dict(template)


def _compound_selector_matches(node: "_Node", selector: str) -> bool:
    selector = selector.strip()
    if not selector:
        return False
    tag_match = re.match(r"^[A-Za-z][A-Za-z0-9_-]*", selector)
    if tag_match and node.tag != tag_match.group(0).lower():
        return False
    rest = selector[tag_match.end():] if tag_match else selector
    while rest:
        if rest.startswith("."):
            match = re.match(r"\.([A-Za-z0-9_-]+)", rest)
            if not match or match.group(1) not in node.attrs.get("class", "").split():
                return False
            rest = rest[len(match.group(0)):]
        elif rest.startswith("#"):
            match = re.match(r"#([A-Za-z0-9_-]+)", rest)
            if not match or node.attrs.get("id") != match.group(1):
                return False
            rest = rest[len(match.group(0)):]
        elif rest.startswith("["):
            end = rest.find("]")
            if end < 0:
                return False
            expression = rest[1:end].strip()
            match = re.fullmatch(r"([A-Za-z_:][-A-Za-z0-9_:.]*)(?:\s*=\s*['\"]?([^'\"]+)['\"]?)?", expression)
            if not match:
                return False
            key, expected = match.group(1).lower(), match.group(2)
            if key not in node.attrs or (expected is not None and node.attrs.get(key) != expected):
                return False
            rest = rest[end + 1:]
        else:
            return False
    return True


def _parse_selector(selector: str) -> List[str]:
    if not isinstance(selector, str) or not selector or len(selector) > MAX_SELECTOR_LENGTH:
        raise TemplateError("invalid HTML selector")
    parts = selector.split()
    if len(parts) > 4:
        raise TemplateError("HTML selectors may contain at most four descendant steps")
    for part in parts:
        if not re.match(r"^(?:[A-Za-z][A-Za-z0-9_-]*)?(?:[.#][A-Za-z0-9_-]+|\[[^\]]+\])*$", part):
            raise TemplateError(f"unsupported CSS-like selector {selector!r}")
        # Validate attribute selector syntax without needing an HTML input.
        probe = _Node("div", {})
        if not _compound_selector_matches(probe, part) and not any(c in part for c in ".#["):
            raise TemplateError(f"invalid CSS-like selector {selector!r}")
    return parts


class _Node:
    __slots__ = ("tag", "attrs", "children", "text", "parent")

    def __init__(self, tag: str, attrs: Dict[str, str], parent: Optional["_Node"] = None):
        self.tag = tag
        self.attrs = attrs
        self.children: List["_Node"] = []
        self.text: List[str] = []
        self.parent = parent


class _TreeParser(HTMLParser):
    VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link",
            "meta", "param", "source", "track", "wbr"}

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.root = _Node("#document", {})
        self.stack = [self.root]
        self.nodes = 0

    def handle_starttag(self, tag: str, attrs):
        self.nodes += 1
        if self.nodes > MAX_HTML_NODES:
            raise TemplateError("HTML source contains too many nodes")
        node = _Node(tag.lower(), {key.lower(): value or "" for key, value in attrs}, self.stack[-1])
        self.stack[-1].children.append(node)
        if tag.lower() not in self.VOID:
            self.stack.append(node)

    def handle_startendtag(self, tag: str, attrs):
        self.handle_starttag(tag, attrs)
        if tag.lower() not in self.VOID and len(self.stack) > 1:
            self.stack.pop()

    def handle_endtag(self, tag: str):
        wanted = tag.lower()
        for index in range(len(self.stack) - 1, 0, -1):
            if self.stack[index].tag == wanted:
                del self.stack[index:]
                break

    def handle_data(self, data: str):
        if data:
            self.stack[-1].text.append(data)


def _selector_matches(node: _Node, parts: Sequence[str]) -> bool:
    if not parts or not _compound_selector_matches(node, parts[-1]):
        return False
    ancestor = node.parent
    for selector in reversed(parts[:-1]):
        while ancestor is not None and not _compound_selector_matches(ancestor, selector):
            ancestor = ancestor.parent
        if ancestor is None:
            return False
        ancestor = ancestor.parent
    return True


def _select(root: _Node, selector: str) -> List[_Node]:
    parts = _parse_selector(selector)
    matches: List[_Node] = []
    stack = list(reversed(root.children))
    while stack:
        node = stack.pop()
        if _selector_matches(node, parts):
            matches.append(node)
        stack.extend(reversed(node.children))
        if len(matches) > MAX_TEMPLATE_ITEMS:
            raise TemplateError("HTML selector matched too many nodes")
    return matches


def _node_text(node: _Node) -> str:
    parts = list(node.text)
    for child in node.children:
        parts.append(_node_text(child))
    return " ".join(" ".join(parts).split())


def _extract_html_field(node: _Node, spec: Dict[str, Any]) -> Any:
    matches = _select(node, str(spec["selector"]))
    multiple = bool(spec.get("multiple", False))
    values = []
    for item in matches:
        attr = spec.get("attr")
        value = item.attrs.get(str(attr).lower(), "") if attr else _node_text(item)
        if value:
            values.append(value)
    return values if multiple else (values[0] if values else spec.get("default"))


def _normalize_fields(fields: Dict[str, Any], fmt: str) -> Dict[str, Any]:
    result = {}
    for key, raw_spec in fields.items():
        spec = _field_spec(raw_spec, fmt)
        result[key] = spec
    return result


def parse_template(data: bytes, template: Any, source_ref: str = "template") -> List[Dict[str, Any]]:
    spec = validate_template(template)
    if len(data) > MAX_TEMPLATE_BYTES:
        raise TemplateError("template source exceeds the 16 MiB input limit")
    fmt = spec["format"]
    fields = _normalize_fields(spec["fields"], fmt)
    records: List[Dict[str, Any]] = []
    if fmt == "json":
        try:
            payload = json.loads(data.decode("utf-8-sig"))
        except (UnicodeError, json.JSONDecodeError, RecursionError) as exc:
            raise TemplateError("template JSON is malformed or too deeply nested") from exc
        raw_records = _json_path_values(payload, spec.get("record_path", "$[*]"))
        if not raw_records and isinstance(payload, list) and spec.get("record_path", "$[*]") == "$[*]":
            raw_records = payload
        for index, item in enumerate(raw_records):
            if not isinstance(item, dict):
                records.append({"_error": "JSON record is not an object", "_source_ref": f"{source_ref}#{index + 1}"})
                continue
            row = {}
            for field, field_spec in fields.items():
                values = _json_path_values(item, field_spec["path"])
                row[field] = values if field_spec.get("multiple") else (values[0] if values else field_spec.get("default"))
            row["_source_ref"] = f"{source_ref}#{index + 1}"
            records.append(row)
            if len(records) > MAX_TEMPLATE_ITEMS:
                raise TemplateError("template JSON contains too many records")
    elif fmt == "html":
        try:
            text = data.decode(str(spec.get("encoding", "utf-8-sig")))
        except (LookupError, UnicodeError) as exc:
            raise TemplateError("template HTML encoding is invalid") from exc
        parser = _TreeParser()
        try:
            parser.feed(text)
            parser.close()
        except Exception as exc:
            if isinstance(exc, TemplateError):
                raise
            raise TemplateError("template HTML parsing failed") from exc
        selector = spec["item_selector"]
        for index, item in enumerate(_select(parser.root, selector)):
            row = {field: _extract_html_field(item, field_spec) for field, field_spec in fields.items()}
            row["_source_ref"] = f"{source_ref}#{index + 1}"
            records.append(row)
            if len(records) > MAX_TEMPLATE_ITEMS:
                raise TemplateError("template HTML contains too many records")
    else:
        try:
            text = data.decode(str(spec.get("encoding", "utf-8-sig")))
        except (LookupError, UnicodeError) as exc:
            raise TemplateError("template text encoding is invalid") from exc
        pattern = _safe_pattern(spec["pattern"])
        lines = text.splitlines()
        for line_no, line in enumerate(lines, 1):
            if len(line.encode("utf-8")) > MAX_TEMPLATE_LINE:
                records.append({"_error": "text line exceeds 16 KiB", "_source_ref": f"{source_ref}:{line_no}"})
                continue
            match = pattern.search(line)
            if not match:
                if spec.get("ignore_unmatched", True):
                    continue
                records.append({"_error": "text line does not match the configured pattern",
                                "_source_ref": f"{source_ref}:{line_no}"})
                continue
            groups = match.groupdict()
            row = {}
            for field, field_spec in fields.items():
                group = field_spec.get("group", field)
                row[field] = groups.get(group, field_spec.get("default"))
            row["_source_ref"] = f"{source_ref}:{line_no}"
            records.append(row)
            if len(records) > MAX_TEMPLATE_ITEMS:
                raise TemplateError("template text contains too many records")
    return records
