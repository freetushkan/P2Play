#!/usr/bin/env python3
"""Independent source inventory for P2Play's catalog grabber.

This module deliberately stores per-source observations and category inventories
without merging or exporting a publisher catalog.
"""
from __future__ import annotations

import json
from pathlib import Path
import re
import sqlite3
import time
import uuid
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

import grabber_lib as g


SNAPSHOT_SOURCE_TYPES = {"csv", "torrents-csv-data", "rutor", "rutracker", "template"}
FEED_SOURCE_TYPES = {"rss", "torznab", "jackett-torznab"}


def _safe_name(value: str) -> str:
    return re.sub(r"[^a-zA-Z0-9_-]+", "_", value).strip("_") or "source"


def _scope(source_type: str) -> str:
    if source_type in FEED_SOURCE_TYPES:
        return "bounded-feed-window"
    return "configured-snapshot"


def _category_model(source_type: str) -> str:
    return {
        "csv": "The torrents-csv-data CSV schema has no explicit category column.",
        "torrents-csv-data": "The torrents-csv-data CSV schema has no explicit category column.",
        "rutor": "Original Rutor Categories values.",
        "rutracker": "RuTracker forum IDs and names.",
        "torznab": "Torznab capabilities plus categories attached to returned items.",
        "jackett-torznab": "Torznab capabilities plus categories attached to returned items.",
        "rss": "Category labels present in RSS items; the feed may omit categories.",
        "template": "Categories selected by the configured source template.",
    }.get(source_type, "No category extractor is defined for this source type.")


def _resolve_sources(profile_path: Path, config: Dict[str, Any]) -> List[Dict[str, Any]]:
    resolved = []
    for item in config.get("sources", []):
        source = dict(item)
        for key in ("repo", "path"):
            if source.get(key):
                path = Path(str(source[key])).expanduser()
                if not path.is_absolute():
                    path = profile_path.expanduser().resolve().parent / path
                source[key] = str(path.resolve())
        resolved.append(source)
    return resolved


def _persist_source(
    db: sqlite3.Connection,
    run_id: str,
    source_id: str,
    snapshot: bool,
    replace_snapshot: bool,
    checkpoint: Optional[Dict[str, Any]],
    checkpoint_deferred: bool,
) -> None:
    """Commit this source independently; no other source's rows are touched."""
    with db:
        if snapshot and replace_snapshot:
            db.execute("DELETE FROM observations WHERE source_id=?", (source_id,))
            db.execute("DELETE FROM inventory_categories WHERE source_id=?", (source_id,))

        db.execute(
            """
            INSERT OR REPLACE INTO observations
              (source_id,ih,priority,record_json,categories_json,source_ref,revision,provenance_json)
            SELECT source_id,ih,priority,record_json,categories_json,source_ref,revision,provenance_json
            FROM run_observations
            WHERE run_id=? AND source_id=?
            """,
            (run_id, source_id),
        )
        db.execute(
            """
            INSERT OR IGNORE INTO inventory_categories(source_id,category_key,label)
            SELECT source_id,category_key,label
            FROM run_categories
            WHERE run_id=? AND source_id=?
            """,
            (run_id, source_id),
        )

        if checkpoint is not None and not checkpoint_deferred:
            db.execute(
                "INSERT OR REPLACE INTO source_checkpoints VALUES(?,?,?)",
                (source_id, g._json(checkpoint), int(time.time())),
            )

        db.execute("DELETE FROM run_observations WHERE run_id=? AND source_id=?", (run_id, source_id))
        db.execute("DELETE FROM run_conflicts WHERE run_id=? AND source_id=?", (run_id, source_id))
        db.execute("DELETE FROM run_categories WHERE run_id=? AND source_id=?", (run_id, source_id))


def _empty_category(name: str) -> Dict[str, Any]:
    return {
        "name": name,
        "category_ids": [],
        "records": 0,
        "deleted_topics": 0,
        "records_with_size": 0,
        "records_without_size": 0,
        "torrent_size_bytes": 0,
    }


def _source_metrics(state: g.State, source_id: str) -> Tuple[Dict[str, Any], List[Dict[str, Any]]]:
    metrics: Dict[str, Any] = {
        "unique_infohashes": 0,
        "active_records": 0,
        "deleted_topics": 0,
        "records_with_size": 0,
        "records_without_size": 0,
        "records_with_categories": 0,
        "records_without_categories": 0,
        "torrent_size_bytes": 0,
        "normalized_record_bytes": 0,
        "category_count": 0,
    }
    categories: Dict[str, Dict[str, Any]] = {}

    taxonomy = state.db.execute(
        "SELECT category_key,label FROM inventory_categories WHERE source_id=? ORDER BY label COLLATE NOCASE",
        (source_id,),
    )
    for row in taxonomy:
        key, label = str(row["category_key"]), str(row["label"])
        category_id = None
        name = label
        if "\t" in label:
            prefix, suffix = label.split("\t", 1)
            if prefix == key:
                category_id, name = prefix, suffix
        norm = name.casefold()
        entry = categories.setdefault(norm, _empty_category(name))
        if category_id and category_id not in entry["category_ids"]:
            entry["category_ids"].append(category_id)

    cursor = state.db.execute(
        "SELECT ih,record_json,categories_json FROM observations WHERE source_id=? ORDER BY ih",
        (source_id,),
    )
    for row in cursor:
        metrics["unique_infohashes"] += 1
        record = json.loads(row["record_json"])
        record_categories = json.loads(row["categories_json"] or "[]")
        is_deleted = "del" in record
        if is_deleted:
            metrics["deleted_topics"] += 1
        else:
            metrics["active_records"] += 1
            metrics["normalized_record_bytes"] += len(row["record_json"].encode("utf-8"))
            size = record.get("size")
            if isinstance(size, int) and not isinstance(size, bool) and size >= 0:
                metrics["records_with_size"] += 1
                metrics["torrent_size_bytes"] += size
            else:
                metrics["records_without_size"] += 1

        names = []
        seen = set()
        for value in record_categories if isinstance(record_categories, list) else []:
            if not isinstance(value, str) or not value.strip():
                continue
            name = value.strip()
            norm = name.casefold()
            if norm in seen:
                continue
            seen.add(norm)
            names.append((norm, name))
        if not is_deleted:
            if names:
                metrics["records_with_categories"] += 1
            else:
                metrics["records_without_categories"] += 1

        for norm, name in names:
            entry = categories.setdefault(norm, _empty_category(name))
            if is_deleted:
                entry["deleted_topics"] += 1
            else:
                entry["records"] += 1
                size = record.get("size")
                if isinstance(size, int) and not isinstance(size, bool) and size >= 0:
                    entry["records_with_size"] += 1
                    entry["torrent_size_bytes"] += size
                else:
                    entry["records_without_size"] += 1

    ordered = sorted(categories.values(), key=lambda item: item["name"].casefold())
    for item in ordered:
        item["category_ids"].sort()
    metrics["category_count"] = len(ordered)
    return metrics, ordered


def _intersections(db: sqlite3.Connection) -> List[Dict[str, Any]]:
    rows = db.execute(
        """
        SELECT a.source_id AS source_a, b.source_id AS source_b, COUNT(*) AS shared_infohashes
        FROM observations a
        JOIN observations b ON a.ih=b.ih AND a.source_id < b.source_id
        WHERE substr(a.record_json,1,7)!='{"del":'
          AND substr(b.record_json,1,7)!='{"del":'
        GROUP BY a.source_id,b.source_id
        ORDER BY a.source_id,b.source_id
        """
    )
    return [
        {"source_a": row["source_a"], "source_b": row["source_b"],
         "shared_infohashes": int(row["shared_infohashes"])}
        for row in rows
    ]


def _write_reports(
    state: g.State,
    report_path: Path,
    categories_dir: Path,
    run_id: str,
    source_configs: Sequence[Dict[str, Any]],
    source_results: Dict[str, Dict[str, Any]],
    selected_ids: Sequence[str],
) -> Dict[str, Any]:
    source_reports: Dict[str, Any] = {}
    category_files: Dict[str, str] = {}

    for source in source_configs:
        source_id = str(source["id"])
        source_type = str(source.get("type") or source_id)
        result = dict(source_results.get(source_id, {}))
        if not result:
            result = {
                "status": "not-selected" if selected_ids else "disabled",
                "reason": "source was not selected for this inventory pass",
            }
        metrics, categories = _source_metrics(state, source_id)
        warnings = []
        if source_type in {"csv", "torrents-csv-data"} and source.get("filters"):
            warnings.append("CSV title filters are enabled; this inventory covers only matching rows.")
        if source_type in FEED_SOURCE_TYPES:
            warnings.append("This is a bounded feed/indexer window, not a complete historical snapshot.")
            if source.get("max_items"):
                warnings.append(f"Configured max_items={source['max_items']}.")
        if source_type == "rutracker" and not source.get("forum_include_ids"):
            warnings.append("Empty forum_include_ids means all RuTracker forums.")
        source_report = {
            "source_id": source_id,
            "type": source_type,
            "scope": _scope(source_type),
            "category_model": _category_model(source_type),
            "status": result.get("status", "not-selected"),
            "reason": result.get("reason"),
            "warnings": warnings,
            "fetched": int(result.get("fetched", 0) or 0),
            "parsed": int(result.get("parsed", 0) or 0),
            "accepted": int(result.get("accepted", 0) or 0),
            "filtered": int(result.get("filtered", 0) or 0),
            "rejected": int(result.get("rejected", 0) or 0),
            "duplicates": int(result.get("duplicates", 0) or 0),
            "conflicts": int(result.get("conflicts", 0) or 0),
            "stale_cache_used": bool(result.get("stale_cache_used", False)),
            "checkpoint_before": result.get("checkpoint_before"),
            "checkpoint_after": result.get("checkpoint") if result.get("status") in {"success", "unchanged"} else None,
            "max_topics": result.get("topic_limit"),
            "metrics": metrics,
            "categories": categories,
        }
        source_reports[source_id] = source_report
        categories_dir.mkdir(parents=True, exist_ok=True)
        category_path = categories_dir / f"{_safe_name(source_id)}.json"
        g._atomic_write(
            category_path,
            (g._json({
                "version": 1,
                "source_id": source_id,
                "type": source_type,
                "category_model": source_report["category_model"],
                "scope": source_report["scope"],
                "status": source_report["status"],
                "metrics": metrics,
                "categories": categories,
            }) + "\n").encode("utf-8"),
        )
        category_files[source_id] = str(category_path)

    totals = state.db.execute(
        """
        SELECT COUNT(*) AS source_observations,
               COUNT(DISTINCT ih) AS unique_across_sources
        FROM observations
        """
    ).fetchone()
    live_unique = state.db.execute(
        """SELECT COUNT(DISTINCT ih) FROM observations
           WHERE substr(record_json,1,7)!='{"del":'"""
    ).fetchone()[0]
    result = {
        "version": 1,
        "run_id": run_id,
        "created_at": int(time.time()),
        "status": "complete",
        "database": str(state.path),
        "sources_selected": list(selected_ids),
        "totals": {
            "source_observations": int(totals["source_observations"]),
            "unique_infohashes_across_sources_including_deletions": int(totals["unique_across_sources"]),
            "unique_live_infohashes_across_sources": int(live_unique),
        },
        "sources": source_reports,
        "source_intersections": _intersections(state.db),
        "category_files": category_files,
        "next_step": "review source reports and intersections; no merged catalog was created",
    }

    failing = any(
        item["status"] in {"failed", "unsupported"} or item["rejected"] or item["conflicts"]
        for item in source_reports.values()
        if item["status"] not in {"disabled", "not-selected"}
    )
    selected_count = sum(item.get("status") not in {"disabled", "not-selected"}
                         for item in source_reports.values())
    if selected_count == 0:
        result["status"] = "no-op"
    elif failing:
        result["status"] = "degraded"

    report_path.parent.mkdir(parents=True, exist_ok=True)
    g._atomic_write(report_path, (g._json(result) + "\n").encode("utf-8"))
    return result


def run_inventory(
    profile_path: Path,
    report_path: Path,
    state_path: Path,
    categories_dir: Optional[Path] = None,
    source_ids: Optional[Sequence[str]] = None,
) -> Dict[str, Any]:
    """Inventory enabled sources independently, without a merged publisher export."""
    profile_path = profile_path.expanduser().resolve()
    report_path = report_path.expanduser().resolve()
    state_path = state_path.expanduser().resolve()
    categories_dir = (categories_dir or (report_path.parent / "categories")).expanduser().resolve()

    config = g._load_config(profile_path)
    sources = _resolve_sources(profile_path, config)
    by_id = {str(item["id"]): item for item in sources}
    requested = list(dict.fromkeys(str(item) for item in (source_ids or [])))
    unknown = sorted(set(requested) - set(by_id))
    if unknown:
        raise g.GrabberError("unknown inventory source ID(s): " + ", ".join(unknown))
    disabled = [source_id for source_id in requested if by_id[source_id].get("enabled", True) is not True]
    if disabled:
        raise g.GrabberError("requested inventory source(s) are disabled in profile: " + ", ".join(disabled))

    selected = set(requested) if requested else {
        str(item["id"]) for item in sources if item.get("enabled", True) is True
    }
    publisher = config.get("publisher", {}) or {}
    state = g.State(state_path)
    state.inventory_mode = True
    run_id = str(uuid.uuid4())
    quarantine_dir = report_path.parent / "quarantine" / run_id
    source_results: Dict[str, Dict[str, Any]] = {}

    try:
        # Inventory uses its own DB. Recover any interrupted inventory staging rows;
        # committed observations/checkpoints are already source-transactional.
        state.db.execute("DELETE FROM runs WHERE status='inventory'")
        state.db.execute(
            "INSERT INTO runs(id,status,created_at) VALUES(?,?,?)",
            (run_id, "inventory", int(time.time())),
        )
        state.db.commit()

        for index, source in enumerate(sources):
            source_id = str(source["id"])
            if source_id not in selected:
                if source.get("enabled", True) is not True:
                    source_results[source_id] = {"status": "disabled", "reason": "source disabled in profile"}
                else:
                    source_results[source_id] = {"status": "not-selected", "reason": "use --source to inventory this source separately"}
                continue

            source_type = str(source.get("type") or source_id)
            before = state.checkpoint(source_id)
            adapter = __import__("grabber_adapters").SOURCE_ADAPTERS.get(source_type)
            if adapter is None:
                source_results[source_id] = {"status": "unsupported", "reason": f"adapter {source_type!r} is not registered"}
                continue

            savepoint = f"inventory_source_{index}"
            state.db.execute(f"SAVEPOINT {savepoint}")
            try:
                result = adapter(
                    source, config.get("templates", {}), state, run_id,
                    publisher, quarantine_dir,
                )
                state.db.execute(f"RELEASE SAVEPOINT {savepoint}")
                result["checkpoint_before"] = before
                result["priority"] = int(source.get("priority", 0))

                if result.get("status") == "success":
                    snapshot = source_type in SNAPSHOT_SOURCE_TYPES
                    complete_snapshot = snapshot and not (
                        result.get("rejected", 0) or result.get("conflicts", 0)
                        or result.get("quarantine_truncated")
                    )
                    checkpoint = result.get("checkpoint")
                    _persist_source(
                        state.db, run_id, source_id, snapshot,
                        complete_snapshot, checkpoint,
                        bool(result.get("quarantine_truncated")),
                    )
                    result["inventory_snapshot_replaced"] = bool(complete_snapshot)
                    if snapshot and not complete_snapshot:
                        result.setdefault("warnings", []).append(
                            "Some source rows were rejected/conflicted; retained prior observations and added valid rows instead of replacing the snapshot."
                        )
                elif result.get("status") == "unchanged":
                    # The prior source snapshot/categories are already in this inventory DB.
                    pass

                cached = state.db.execute(
                    "SELECT COUNT(*) FROM observations WHERE source_id=?", (source_id,)
                ).fetchone()[0]
                if result.get("status") in {"failed", "skipped"} and cached:
                    result["stale_cache_used"] = True
                source_results[source_id] = result
            except (g.SourceError, g.GrabberError, OSError, ValueError, TypeError) as exc:
                state.db.execute(f"ROLLBACK TO SAVEPOINT {savepoint}")
                state.db.execute(f"RELEASE SAVEPOINT {savepoint}")
                cached = state.db.execute(
                    "SELECT COUNT(*) FROM observations WHERE source_id=?", (source_id,)
                ).fetchone()[0]
                source_results[source_id] = {
                    "status": "failed",
                    "reason": str(exc),
                    "checkpoint_before": before,
                    "priority": int(source.get("priority", 0)),
                    "stale_cache_used": bool(cached),
                    "fetched": 0,
                    "parsed": 0,
                    "accepted": 0,
                    "filtered": 0,
                    "rejected": 0,
                    "duplicates": 0,
                    "errors": [],
                }

        report = _write_reports(
            state, report_path, categories_dir, run_id, sources,
            source_results, sorted(selected),
        )
        state.db.execute("DELETE FROM runs WHERE id=?", (run_id,))
        state.db.commit()
        return report
    finally:
        try:
            state.db.execute("DELETE FROM runs WHERE id=?", (run_id,))
            state.db.commit()
        except sqlite3.Error:
            pass
        state.close()
