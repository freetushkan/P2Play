#!/usr/bin/env python3
"""Independent source inventory for P2Play's catalog grabber.

This module deliberately stores per-source observations and category inventories
without merging or exporting a publisher catalog.
"""
from __future__ import annotations

import html
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


def _category_model(source_type: str, source: Optional[Dict[str, Any]] = None) -> str:
    if source_type in {"csv", "torrents-csv-data"} and source and source.get("inventory_categories"):
        return "Heuristic title-pattern groups configured under inventory_categories; not native CSV taxonomy."
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
        "records_matching_multiple_categories": 0,
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
                if len(names) > 1:
                    metrics["records_matching_multiple_categories"] += 1
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


def _rutracker_group_summary(state: g.State, source: Dict[str, Any]) -> Dict[str, Any]:
    """Derive groups from stored forum IDs, never from potentially ambiguous labels."""
    configured = source.get("category_groups", {})
    groups = {name: {**_empty_category(name), "category_ids": [str(i) for i in ids]}
              for name, ids in configured.items()}
    by_id = {str(i): name for name, ids in configured.items() for i in ids}
    ungrouped = _empty_category("Ungrouped")
    for row in state.db.execute(
        "SELECT record_json,provenance_json FROM observations WHERE source_id=? ORDER BY ih",
        (str(source["id"]),),
    ):
        record = json.loads(row["record_json"])
        provenance = json.loads(row["provenance_json"] or "{}")
        group = by_id.get(str(provenance.get("forum_id", "")))
        entry = groups[group] if group is not None else ungrouped
        if "del" in record:
            entry["deleted_topics"] += 1
            continue
        entry["records"] += 1
        size = record.get("size")
        if isinstance(size, int) and not isinstance(size, bool) and size >= 0:
            entry["records_with_size"] += 1
            entry["torrent_size_bytes"] += size
        else:
            entry["records_without_size"] += 1
    return {"groups": list(groups.values()), "ungrouped": ungrouped,
            "basis": "current profile category_groups applied to stored provenance forum_id"}


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


def _source_coverage(db: sqlite3.Connection, source_ids: Sequence[str]) -> List[Dict[str, Any]]:
    """Coverage by the UNION of other stored live inventories, not pairwise sums."""
    counts = {}
    for row in db.execute(
        """SELECT a.source_id, COUNT(*) AS active,
                  SUM(CASE WHEN EXISTS (
                      SELECT 1 FROM observations b
                      WHERE b.ih=a.ih AND b.source_id!=a.source_id
                        AND substr(b.record_json,1,7)!='{"del":'
                  ) THEN 1 ELSE 0 END) AS covered
           FROM observations a
           WHERE substr(a.record_json,1,7)!='{"del":'
           GROUP BY a.source_id ORDER BY a.source_id"""
    ):
        counts[row["source_id"]] = (int(row["active"]), int(row["covered"]))
    result = []
    for source_id in sorted(set(source_ids) | set(counts)):
        active, covered = counts.get(source_id, (0, 0))
        result.append({
            "source_id": source_id,
            "active_infohashes": active,
            "covered_by_other_sources": covered,
            "coverage_percent": round(100 * covered / active, 4) if active else None,
            "exclusive_infohashes": active - covered,
        })
    return result


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
        if source_type in {"csv", "torrents-csv-data"} and source.get("inventory_categories"):
            warnings.append(
                "CSV groups are heuristic title matches, not source-native categories; "
                + ("first matching group wins." if source.get("inventory_category_mode") == "first"
                   else "a record can match multiple groups.")
            )
        if source_type in FEED_SOURCE_TYPES:
            warnings.append("This is a bounded feed/indexer window, not a complete historical snapshot.")
            if source.get("max_items"):
                warnings.append(f"Configured max_items={source['max_items']}.")
        if source_type == "rutracker" and not source.get("forum_include_ids"):
            warnings.append("Empty forum_include_ids means all RuTracker forums.")
        if source_type == "rutor":
            warnings.append("Native Rutor categories are reported as-is; Anime remains separate and is not merged into Movie/Series.")
        source_report = {
            "source_id": source_id,
            "type": source_type,
            "scope": _scope(source_type),
            "category_model": _category_model(source_type, source),
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
            "filter_reasons": dict(result.get("filter_reasons", {}) or {}),
            "stale_cache_used": bool(result.get("stale_cache_used", False)),
            "checkpoint_before": result.get("checkpoint_before"),
            "checkpoint_after": (
                result.get("checkpoint")
                if result.get("status") in {"success", "unchanged"}
                and not result.get("checkpoint_deferred") else None
            ),
            "checkpoint_deferred": bool(result.get("checkpoint_deferred")),
            "snapshot_replaced": result.get("inventory_snapshot_replaced"),
            "quarantined": int(result.get("quarantined", 0) or 0),
            "quarantine_truncated": bool(result.get("quarantine_truncated", False)),
            "adapter_warnings": list(result.get("warnings", [])),
            "max_topics": result.get("topic_limit"),
            "metrics": metrics,
            "categories": categories,
        }
        if source_type == "rutracker":
            source_report["category_group_summary"] = _rutracker_group_summary(state, source)
            categories_dir.mkdir(parents=True, exist_ok=True)
            deleted_path = categories_dir / f"{_safe_name(source_id)}-deleted.ndjson"
            temporary = deleted_path.with_suffix(".tmp")
            try:
                with temporary.open("w", encoding="utf-8") as stream:
                    for row in state.db.execute(
                        "SELECT ih,record_json,provenance_json FROM observations WHERE source_id=? ORDER BY ih",
                        (source_id,),
                    ):
                        if "del" in json.loads(row["record_json"]):
                            stream.write(g._json({"ih": row["ih"],
                                **json.loads(row["provenance_json"])}) + "\n")
                temporary.replace(deleted_path)
            finally:
                temporary.unlink(missing_ok=True)
            source_report["deleted_file"] = str(deleted_path)
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
                **({"category_group_summary": source_report["category_group_summary"]}
                   if "category_group_summary" in source_report else {}),
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
        "source_coverage": _source_coverage(state.db, list(source_reports)),
        "source_coverage_scope": "all stored live inventories, including cached/unselected/disabled sources",
        "category_files": category_files,
        "next_step": "review source reports and intersections; no merged catalog was created",
    }

    failing = any(
        item["status"] in {"failed", "unsupported", "skipped"}
        or item["rejected"] or item["conflicts"]
        for item in source_reports.values()
        if item["status"] not in {"disabled", "not-selected"}
    )
    selected_count = sum(item.get("status") not in {"disabled", "not-selected"}
                         for item in source_reports.values())
    if selected_count == 0:
        result["status"] = "no-op"
    elif failing:
        result["status"] = "degraded"

    html_path = report_path.with_suffix(".html")
    result["html_report"] = str(html_path)
    report_path.parent.mkdir(parents=True, exist_ok=True)
    g._atomic_write(report_path, (g._json(result) + "\n").encode("utf-8"))
    g._atomic_write(html_path, _render_html(result).encode("utf-8"))
    return result


def _format_bytes(value: Any) -> str:
    try:
        number = max(0, int(value))
    except (TypeError, ValueError, OverflowError):
        return "—"
    units = ("B", "KiB", "MiB", "GiB", "TiB")
    amount = float(number)
    for unit in units:
        if amount < 1024 or unit == units[-1]:
            return f"{amount:,.1f} {unit}" if unit != "B" else f"{number:,} B"
        amount /= 1024
    return f"{number:,} B"


def _group_html_row(entry: Dict[str, Any], esc, label: Optional[str] = None) -> str:
    return (
        f"<tr><td>{esc(label if label is not None else entry.get('name'))}</td>"
        f"<td>{esc(', '.join(entry.get('category_ids', [])) or '—')}</td>"
        f"<td class='num'>{int(entry.get('records', 0)):,}</td>"
        f"<td class='num'>{int(entry.get('deleted_topics', 0)):,}</td>"
        f"<td class='num'>{int(entry.get('records_with_size', 0)):,}</td>"
        f"<td class='num'>{int(entry.get('records_without_size', 0)):,}</td>"
        f"<td class='num'>{_format_bytes(entry.get('torrent_size_bytes'))}</td></tr>"
    )


def _render_html(report: Dict[str, Any]) -> str:
    esc = lambda value: html.escape(str(value if value is not None else "—"), quote=True)
    created = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(int(report.get("created_at", 0))))
    sources = report.get("sources", {})
    parts = [
        "<!doctype html><html lang='en'><head><meta charset='utf-8'>",
        "<meta name='viewport' content='width=device-width, initial-scale=1'>",
        "<title>P2Play source inventory</title><style>",
        ":root{color-scheme:light dark;font:15px/1.5 system-ui,sans-serif}",
        "body{max-width:1500px;margin:0 auto;padding:24px;color:CanvasText;background:Canvas}",
        "h1{margin-bottom:4px}h2{margin-top:0}small,.muted{color:GrayText;font-weight:400}",
        "section{border:1px solid color-mix(in srgb,CanvasText 18%,transparent);border-radius:12px;padding:18px;margin:24px 0}",
        ".summary,.metrics{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:12px;margin:18px 0}",
        ".summary div,.metrics div{background:color-mix(in srgb,CanvasText 6%,Canvas);border-radius:8px;padding:12px}",
        ".summary b,.metrics b{display:block;font-size:1.25rem;font-variant-numeric:tabular-nums}",
        ".summary span,.metrics span{color:GrayText;font-size:.85rem}",
        ".table-wrap{overflow-x:auto}table{border-collapse:collapse;width:100%;font-size:.92rem}",
        "th,td{text-align:left;padding:9px 10px;border-bottom:1px solid color-mix(in srgb,CanvasText 14%,transparent);white-space:nowrap}",
        ".num{text-align:right;font-variant-numeric:tabular-nums}",
        ".status{display:inline-block;border-radius:20px;padding:2px 9px;background:color-mix(in srgb,CanvasText 12%,Canvas);font-size:.8rem}",
        ".status.success,.status.complete{background:#176b3a;color:white}.status.degraded,.status.failed{background:#97530b;color:white}",
        ".bar{width:140px;height:8px;background:color-mix(in srgb,CanvasText 12%,Canvas);border-radius:20px;overflow:hidden}",
        ".bar span{display:block;height:100%;background:#4285d4;border-radius:20px}",
        ".warnings{padding-left:22px;color:#c58b32}.foot{margin-top:32px;color:GrayText;font-size:.85rem}",
        "@media(max-width:650px){body{padding:12px}section{padding:12px}.summary,.metrics{grid-template-columns:repeat(2,minmax(0,1fr))}}",
        "</style></head><body>",
        "<h1>P2Play source inventory</h1>",
        f"<p class='muted'>Generated {esc(created)} · run {esc(report.get('run_id'))} · status <span class='status {esc(report.get('status'))}'>{esc(report.get('status'))}</span></p>",
    ]

    totals = report.get("totals", {})
    parts.append(
        "<div class='summary'>"
        f"<div><b>{int(totals.get('source_observations', 0)):,}</b><span>source observations</span></div>"
        f"<div><b>{int(totals.get('unique_live_infohashes_across_sources', 0)):,}</b><span>unique live hashes across sources</span></div>"
        f"<div><b>{int(totals.get('unique_infohashes_across_sources_including_deletions', 0)):,}</b><span>unique hashes incl. deletions</span></div>"
        f"<div><b>{len(sources):,}</b><span>configured sources in report</span></div>"
        "</div>"
    )

    overview_rows = []
    for source_id, source in sources.items():
        metrics = source.get("metrics", {})
        status = source.get("status", "unknown")
        overview_rows.append(
            "<tr>"
            f"<td>{esc(source_id)}</td><td><span class='status {esc(status)}'>{esc(status)}</span></td>"
            f"<td class='num'>{int(metrics.get('active_records', 0)):,}</td>"
            f"<td class='num'>{int(metrics.get('deleted_topics', 0)):,}</td>"
            f"<td class='num'>{int(metrics.get('category_count', 0)):,}</td>"
            f"<td class='num'>{_format_bytes(metrics.get('torrent_size_bytes'))}</td>"
            f"<td class='num'>{_format_bytes(metrics.get('normalized_record_bytes'))}</td>"
            "</tr>"
        )
    parts.append(
        "<section><h2>Source overview</h2><div class='table-wrap'><table><thead><tr>"
        "<th>Source</th><th>Status</th><th>Active</th><th>Deleted</th><th>Categories</th>"
        "<th>Payload sum</th><th>Normalized JSON</th></tr></thead><tbody>"
        + "".join(overview_rows)
        + "</tbody></table></div><p class='muted'>Category totals can overlap when one item belongs to multiple categories. Source taxonomies are not merged.</p></section>"
    )

    for source_id, source in sources.items():
        metrics = source.get("metrics", {})
        categories = source.get("categories", [])
        maximum = max((int(row.get("records", 0)) for row in categories), default=0)
        category_rows = []
        for category in categories:
            count = int(category.get("records", 0))
            width = (count / maximum * 100) if maximum else 0
            ids = ", ".join(category.get("category_ids", []) or [])
            category_rows.append(
                "<tr>"
                f"<td>{esc(category.get('name'))}</td><td>{esc(ids or '—')}</td>"
                f"<td class='num'>{count:,}</td>"
                f"<td class='num'>{int(category.get('deleted_topics', 0)):,}</td>"
                f"<td class='num'>{int(category.get('records_with_size', 0)):,}</td>"
                f"<td class='num'>{int(category.get('records_without_size', 0)):,}</td>"
                f"<td class='num'>{_format_bytes(category.get('torrent_size_bytes'))}</td>"
                f"<td><div class='bar'><span style='width:{width:.2f}%'></span></div></td>"
                "</tr>"
            )
        if not category_rows:
            category_rows.append("<tr><td colspan='8' class='muted'>No category observations in this source.</td></tr>")

        warnings = list(source.get("warnings", [])) + list(source.get("adapter_warnings", []))
        if source.get("reason"):
            warnings.append(str(source["reason"]))
        if source.get("stale_cache_used"):
            warnings.append("Showing last successful source inventory; latest refresh failed or was skipped.")
        if source.get("filter_reasons"):
            warnings.append("Title filter counters: " + ", ".join(
                f"{key}={value}" for key, value in sorted(source["filter_reasons"].items())
            ))
        warning_html = (
            "<ul class='warnings'>" + "".join(f"<li>{esc(item)}</li>" for item in warnings) + "</ul>"
            if warnings else ""
        )
        group_html = ""
        summary = source.get("category_group_summary")
        if summary is not None:
            group_rows = []
            for entry in summary.get("groups", []):
                group_rows.append(_group_html_row(entry, esc))
            ungrouped = summary.get("ungrouped", {})
            group_rows.append(_group_html_row(ungrouped, esc, "Ungrouped (unknown/missing forum ID or no rule)"))
            group_html = (
                "<h3>RuTracker category groups</h3><div class='table-wrap'><table><thead><tr>"
                "<th>Group</th><th>Configured forum IDs</th><th>Active</th><th>Deleted</th>"
                "<th>With size</th><th>Without size</th><th>Payload size sum</th>"
                "</tr></thead><tbody>" + "".join(group_rows) + "</tbody></table></div>"
                "<p class='muted'>Current profile category_groups applied to stored forum IDs. "
                "Each hash belongs to one group or Ungrouped; deleted entries contribute no payload size. "
                "Native forum taxonomy below is unchanged. Empty configured groups remain visible. "
                "Cached inventories reflect their last scan and forum filters; this summary does not rescan XML.</p>"
            )
        parts.append(
            f"<section><h2>{esc(source_id)} <small>({esc(source.get('type'))})</small></h2>"
            f"<p class='muted'>{esc(source.get('category_model', ''))} · scope: {esc(source.get('scope'))}</p>"
            "<div class='metrics'>"
            f"<div><b>{int(metrics.get('active_records', 0)):,}</b><span>active records</span></div>"
            f"<div><b>{int(metrics.get('unique_infohashes', 0)):,}</b><span>unique hashes incl. deletions</span></div>"
            f"<div><b>{_format_bytes(metrics.get('torrent_size_bytes'))}</b><span>torrent payload size sum</span></div>"
            f"<div><b>{_format_bytes(metrics.get('normalized_record_bytes'))}</b><span>normalized record JSON</span></div>"
            f"</div>{warning_html}{group_html}"
            "<div class='table-wrap'><table><thead><tr>"
            "<th>Category / forum</th><th>Native ID(s)</th><th>Records</th><th>Deleted topics</th>"
            "<th>With size</th><th>Without size</th><th>Payload size sum</th><th>Relative count</th>"
            "</tr></thead><tbody>" + "".join(category_rows) + "</tbody></table></div>"
            f"<p class='muted'>With categories: {int(metrics.get('records_with_categories', 0)):,}; "
            f"without categories: {int(metrics.get('records_without_categories', 0)):,}; "
            f"matching multiple categories: {int(metrics.get('records_matching_multiple_categories', 0)):,}; "
            f"without size: {int(metrics.get('records_without_size', 0)):,}; "
            f"rejected: {int(source.get('rejected', 0)):,}; quarantined: {int(source.get('quarantined', 0)):,}.</p>"
            "</section>"
        )

    intersections = report.get("source_intersections", [])
    intersection_rows = [
        f"<tr><td>{esc(row.get('source_a'))}</td><td>{esc(row.get('source_b'))}</td>"
        f"<td class='num'>{int(row.get('shared_infohashes', 0)):,}</td></tr>"
        for row in intersections
    ] or ["<tr><td colspan='3' class='muted'>No shared live infohashes found so far.</td></tr>"]
    parts.append(
        "<section><h2>Cross-source intersections</h2><div class='table-wrap'><table><thead><tr>"
        "<th>Source A</th><th>Source B</th><th>Shared live infohashes</th></tr></thead><tbody>"
        + "".join(intersection_rows)
        + "</tbody></table></div><p class='muted'>Counts only: no source precedence, winner selection or cross-source merge is performed.</p></section>"
    )
    coverage_rows = []
    for row in report.get("source_coverage", []):
        percent = row.get("coverage_percent")
        percentage = f"{float(percent):.2f}%" if percent is not None else "—"
        coverage_rows.append(
            f"<tr><td>{esc(row.get('source_id'))}</td>"
            f"<td class='num'>{int(row.get('active_infohashes', 0)):,}</td>"
            f"<td class='num'>{int(row.get('covered_by_other_sources', 0)):,}</td>"
            f"<td class='num'>{percentage}</td>"
            f"<td class='num'>{int(row.get('exclusive_infohashes', 0)):,}</td></tr>"
        )
    parts.append(
        "<section><h2>Source coverage by all other sources</h2>"
        "<div class='table-wrap'><table><thead><tr><th>Source</th>"
        "<th>Active hashes</th><th>Covered by others (union)</th>"
        "<th>Coverage</th><th>Exclusive hashes</th></tr></thead><tbody>"
        + ("".join(coverage_rows) or "<tr><td colspan='5'>No inventory data.</td></tr>")
        + "</tbody></table></div><p class='muted'>Each live hash counts once, even if present "
        "in several other sources. Deleted observations never count. The denominator is this "
        "source's active hashes; empty sources have no percentage. Uses all stored inventories, "
        "including cached, unselected and disabled sources (also sources removed from the profile). "
        "Consult source statuses for freshness; coverage does not imply equal metadata or availability.</p></section>"
    )
    parts.append("<p class='foot'>Inventory only. No list, shard, publishing or subscription decision is implied.</p></body></html>")
    return "".join(parts)


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
                    checkpoint_deferred = bool(
                        result.get("quarantine_truncated") or (snapshot and not complete_snapshot)
                    )
                    _persist_source(
                        state.db, run_id, source_id, snapshot,
                        complete_snapshot, checkpoint, checkpoint_deferred,
                    )
                    result["inventory_snapshot_replaced"] = bool(complete_snapshot)
                    result["checkpoint_deferred"] = checkpoint_deferred
                    if snapshot and not complete_snapshot:
                        result["status"] = "degraded"
                        result.setdefault("warnings", []).append(
                            "Some source rows were rejected/conflicted; retained prior observations and added valid rows instead of replacing the snapshot. The source checkpoint was not advanced, so the same snapshot can be retried after correcting the cause."
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
