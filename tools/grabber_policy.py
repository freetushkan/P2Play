"""Small data-only source policies, independent of publishing and transport."""
from __future__ import annotations

import hashlib
import json
from typing import Any, Dict, Optional


def validate_source_policy(source: Dict[str, Any]) -> None:
    groups = source.get("category_groups", {})
    if not isinstance(groups, dict) or len(groups) > 32:
        raise ValueError("category_groups must be an object with at most 32 groups")
    seen = set()
    for name, ids in groups.items():
        if not isinstance(name, str) or not name or len(name) > 128:
            raise ValueError("category group names must contain 1..128 characters")
        if not isinstance(ids, list) or not ids:
            raise ValueError("category groups require non-empty forum ID arrays")
        for value in ids:
            value = str(value)
            if not value.isdigit() or value in seen:
                raise ValueError("forum IDs must be numeric and belong to only one group")
            seen.add(value)
    extras = source.get("extra_fields", {})
    if not isinstance(extras, dict) or any(
        key not in {"description", "history", "duplicate_hints"} or not isinstance(value, bool)
        for key, value in extras.items()
    ):
        raise ValueError("extra_fields supports boolean description/history/duplicate_hints only")
    if source.get("inventory_category_mode", "all") not in {"all", "first"}:
        raise ValueError("inventory_category_mode must be all or first")


def policy_revision(source: Dict[str, Any]) -> str:
    # Reprocess unchanged input when normalization/filter policy changes. Paths,
    # limits and credentials deliberately do not enter the persisted fingerprint.
    keys = ("category_groups", "extra_fields", "forum_include_ids", "filters",
            "inventory_categories", "inventory_category_mode", "date_field")
    payload = {key: source[key] for key in keys if key in source}
    # Group order is semantic in first-match mode, unlike ordinary object keys.
    if "inventory_categories" in payload:
        payload["inventory_categories"] = list(payload["inventory_categories"].items())
    return hashlib.sha256(json.dumps(payload, sort_keys=True).encode()).hexdigest()


def forum_group(source: Dict[str, Any], forum_id: str) -> Optional[str]:
    if "_forum_groups" in source:
        return source["_forum_groups"].get(str(forum_id))
    for name, ids in source.get("category_groups", {}).items():
        if str(forum_id) in {str(value) for value in ids}:
            return name
    return None
