#!/usr/bin/env python3
"""Small, deterministic tests for the review-first catalog grabber."""
from __future__ import annotations

import io
import json
import lzma
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import urllib.parse
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
import grabber_lib as g
import grabber_inventory as gi
import grabber_templates as gt
import grabber_adapters as ga


class GrabberTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="p2play-grabber-test-")
        self.root = Path(self.temp.name)
        self.repo = self.root / "csv-repo"
        self.repo.mkdir()
        subprocess.run(["git", "init", str(self.repo)], check=True, capture_output=True)
        subprocess.run(["git", "-C", str(self.repo), "config", "user.email", "grabber-test@example.invalid"], check=True)
        subprocess.run(["git", "-C", str(self.repo), "config", "user.name", "Grabber Test"], check=True)
        self.csv_path = self.repo / "torrents.csv"
        fixtures = Path(__file__).resolve().parent.parent / "fixtures" / "grabber"
        self.csv_path.write_bytes((fixtures / "torrents.csv").read_bytes())
        subprocess.run(["git", "-C", str(self.repo), "add", "torrents.csv"], check=True)
        subprocess.run(["git", "-C", str(self.repo), "commit", "-m", "fixture"], check=True, capture_output=True)

        fixtures = Path(__file__).resolve().parent.parent / "fixtures" / "grabber"
        self.rutor_json = json.loads((fixtures / "rutor.json").read_text(encoding="utf-8"))
        self.rutor_path = self.root / "rutor.ls"
        self.write_rutor()
        self.rutracker_xml = (fixtures / "rutracker.xml").read_bytes()
        self.rutracker_path = self.root / "rutracker-20261031.xml.xz"
        self.rutracker_path.write_bytes(lzma.compress(self.rutracker_xml))
        self.torznab_caps = (fixtures / "torznab-caps.xml").read_bytes()
        self.torznab_feed = (fixtures / "torznab-feed.xml").read_bytes()
        self.profile = self.root / "profile.json"
        self.write_profile()
        self.full = self.root / "work" / "catalog-full.ndjson"
        self.delta = self.root / "work" / "catalog-delta.ndjson"
        self.report = self.root / "work" / "report.json"
        self.state = self.root / "work" / "grabber.sqlite3"
        self.dump = self.root / "work" / "categories"

    def tearDown(self):
        self.temp.cleanup()

    def write_rutor(self):
        raw = json.dumps(self.rutor_json, ensure_ascii=False).encode("utf-8")
        encoder = zlib.compressobj(level=9, wbits=-15)
        self.rutor_path.write_bytes(encoder.compress(raw) + encoder.flush())

    def write_profile(self, csv_enabled=True, rutor_enabled=True):
        config = {
            "version": 1,
            "publisher": {
                "category_map": {"Movies": "movie"},
                "shard_policy": {"default": "default", "category_map": {"movie": "films"}},
            },
            "sources": [
                {"id": "torrents-csv-data", "type": "csv", "enabled": csv_enabled,
                 "priority": 10, "repo": str(self.repo), "data_file": "torrents.csv",
                 "filters": {"title_regex": {"mode": "any", "include": [r"\.(?:mkv|mp4)$"], "exclude": []}}},
                {"id": "rutor", "type": "rutor", "enabled": rutor_enabled,
                 "priority": 20, "path": str(self.rutor_path), "size_unit": "binary"},
            ],
        }
        self.profile.write_text(json.dumps(config), encoding="utf-8")

    def run_once(self):
        return g.run_grabber(self.profile, self.full, self.delta, self.report, self.state, self.dump)

    def start_http_server(self, handler_class):
        server = ThreadingHTTPServer(("127.0.0.1", 0), handler_class)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        def stop_server():
            server.shutdown()
            worker.join(timeout=5)
            server.server_close()
        self.addCleanup(stop_server)
        return server

    def read_rows(self, path):
        return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]

    def test_inventory_runs_sources_separately_and_reports_categories_and_overlap(self):
        config = json.loads(self.profile.read_text(encoding="utf-8"))
        csv_source = next(item for item in config["sources"] if item["id"] == "torrents-csv-data")
        csv_source["inventory_categories"] = {
            "Video": [r"(?i)\.(?:mkv|mp4)$"],
            "Audio": [r"(?i)\b(?:FLAC|MP3)\b"],
        }
        csv_source["filters"]["title_regex"]["include"].append(r"(?i)\b(?:FLAC|MP3)\b")
        with self.csv_path.open("a", encoding="utf-8") as csv_stream:
            csv_stream.write("eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee,\"Live album FLAC\",300000000,1700000300,1,0,0,0,0\n")
        config["sources"].append({
            "id": "rutracker", "type": "rutracker", "enabled": True,
            "priority": 30, "path": str(self.rutracker_path),
            "forum_include_ids": [], "max_topics": 10,
        })
        self.profile.write_text(json.dumps(config), encoding="utf-8")
        inventory_root = self.root / "inventory"
        report_path = inventory_root / "inventory-report.json"
        state_path = inventory_root / "inventory.sqlite3"
        categories_dir = inventory_root / "categories"

        report = gi.run_inventory(
            self.profile, report_path, state_path, categories_dir,
        )
        self.assertEqual(report["status"], "complete")
        self.assertFalse((inventory_root / "catalog-full.ndjson").exists())
        self.assertFalse((inventory_root / "catalog-delta.ndjson").exists())

        csv = report["sources"]["torrents-csv-data"]
        rutor = report["sources"]["rutor"]
        rutracker = report["sources"]["rutracker"]
        self.assertEqual(csv["status"], "success")
        self.assertEqual(csv["metrics"]["active_records"], 1)
        self.assertEqual(csv["metrics"]["category_count"], 2)
        self.assertIn("heuristic", csv["category_model"].lower())
        csv_categories = {item["name"]: item for item in csv["categories"]}
        self.assertEqual(csv_categories["Video"]["records"], 1)
        self.assertEqual(csv_categories["Audio"]["records"], 1)
        self.assertTrue(report["html_report"].endswith("inventory-report.html"))
        html_report = Path(report["html_report"]).read_text(encoding="utf-8")
        self.assertIn("Source overview", html_report)
        self.assertIn("Video", html_report)
        self.assertIn("Audio", html_report)
        self.assertEqual(rutor["status"], "success")
        self.assertEqual(rutor["metrics"]["active_records"], 2)
        self.assertEqual(rutor["metrics"]["category_count"], 1)
        movies = next(x for x in rutor["categories"] if x["name"] == "Movies")
        self.assertEqual(movies["records"], 2)
        self.assertGreater(movies["torrent_size_bytes"], 0)
        self.assertEqual(rutracker["status"], "success")
        self.assertEqual(rutracker["metrics"]["active_records"], 1)
        self.assertEqual(rutracker["metrics"]["deleted_topics"], 1)
        self.assertEqual(rutracker["metrics"]["category_count"], 1)
        self.assertEqual(next(x for x in rutracker["categories"] if x["name"] == "Movies")["records"], 1)

        shared = {
            (x["source_a"], x["source_b"]): x["shared_infohashes"]
            for x in report["source_intersections"]
        }
        self.assertEqual(shared[("rutor", "torrents-csv-data")], 1)
        self.assertTrue((categories_dir / "rutracker.json").is_file())

        # A second pass against unchanged snapshot sources reuses their independent inventory.
        repeated = gi.run_inventory(
            self.profile, report_path, state_path, categories_dir,
            source_ids=["rutracker"],
        )
        self.assertEqual(repeated["sources"]["rutracker"]["status"], "unchanged")
        self.assertEqual(repeated["sources"]["rutor"]["status"], "not-selected")
        self.assertEqual(repeated["sources"]["rutor"]["metrics"]["active_records"], 2)

        # A failed refresh must not erase the last successful source inventory.
        broken = json.loads(self.profile.read_text(encoding="utf-8"))
        for source in broken["sources"]:
            if source["id"] == "rutor":
                source["path"] = str(self.root / "missing-rutor.ls")
        self.profile.write_text(json.dumps(broken), encoding="utf-8")
        failed = gi.run_inventory(
            self.profile, report_path, state_path, categories_dir,
            source_ids=["rutor"],
        )
        self.assertEqual(failed["sources"]["rutor"]["status"], "failed")
        self.assertTrue(failed["sources"]["rutor"]["stale_cache_used"])
        self.assertEqual(failed["sources"]["rutor"]["metrics"]["active_records"], 2)

    def test_csv_rutor_merge_and_category_dump(self):
        report = self.run_once()
        rows = self.read_rows(self.full)
        by_hash = {row["ih"]: row for row in rows}
        self.assertEqual(report["status"], "complete")
        self.assertEqual(len(rows), 2)
        self.assertNotIn("b" * 40, by_hash)  # title regex filter
        self.assertEqual(by_hash["a" * 40]["title"], "Higher priority title")
        self.assertEqual(by_hash["a" * 40]["shard"], "films")
        self.assertEqual(by_hash["a" * 40]["size"], 2 * 1024 * 1024)
        self.assertEqual(by_hash["c" * 40]["size"], int(1.5 * 1024 ** 3))
        self.assertEqual(by_hash["c" * 40]["date"], 1704157445)
        self.assertEqual(len(by_hash["c" * 40]["trackers"]), 2)
        self.assertEqual(by_hash["c" * 40]["tags"], ["movie"])
        self.assertEqual(self.read_rows(self.delta), rows)
        self.assertEqual((self.dump / "rutor.txt").read_text(encoding="utf-8"), "Movies\n")
        self.assertTrue((self.root / "work" / "pending-run.json").is_file())

    def test_example_csv_title_filter_includes_audio_and_excludes_iso(self):
        profile_path = Path(__file__).resolve().parent / "grabber-profile.example.json"
        config = json.loads(profile_path.read_text(encoding="utf-8"))
        source = next(item for item in config["sources"] if item["id"] == "torrents-csv-data")
        compiled = g._compile_title_filter(source)
        self.assertTrue(g._title_allowed("Artist - Album FLAC", compiled))
        self.assertTrue(g._title_allowed("Artist - Album DSD256", compiled))
        self.assertTrue(g._title_allowed("Artist - Album SACD-R", compiled))
        self.assertTrue(g._title_allowed("Movie.release.mkv", compiled))
        self.assertFalse(g._title_allowed("Disc image.iso", compiled))

    def test_ack_delta_then_unchanged_run_is_idempotent(self):
        first = self.run_once()
        ack = g.ack_run(self.state, first["run_id"], self.full, 2, "auto")
        self.assertEqual(ack["mode"], "delta")
        second = self.run_once()
        self.assertEqual(second["counts"], {"full": 2, "delta": 0})
        self.assertEqual(second["sources"]["torrents-csv-data"]["status"], "unchanged")
        self.assertEqual(second["sources"]["rutor"]["status"], "unchanged")
        ack2 = g.ack_run(self.state, second["run_id"], self.full, 0, "auto")
        self.assertEqual(ack2["accepted_count"], 0)

    def test_changed_worktree_csv_produces_delta_and_checkpoint_waits_for_ack(self):
        self.write_profile(rutor_enabled=False)
        first = self.run_once()
        g.ack_run(self.state, first["run_id"], self.full, 1, "auto")
        old_checkpoint = g.State(self.state).checkpoint("torrents-csv-data")
        csv = self.csv_path.read_text(encoding="utf-8").replace("CSV title only.mkv", "Updated CSV title.mkv")
        self.csv_path.write_text(csv, encoding="utf-8")  # dirty worktree; no commit required
        second = self.run_once()
        self.assertEqual(second["counts"]["delta"], 1)
        delta = self.read_rows(self.delta)
        self.assertEqual(delta[0]["title"], "Updated CSV title.mkv")
        current_before_ack = g.State(self.state).checkpoint("torrents-csv-data")
        self.assertEqual(current_before_ack, old_checkpoint)
        g.ack_run(self.state, second["run_id"], self.full, 1, "delta")
        current_after_ack = g.State(self.state).checkpoint("torrents-csv-data")
        self.assertNotEqual(current_after_ack, old_checkpoint)

    def test_failed_source_isolated_and_last_known_good_reused(self):
        first = self.run_once()
        g.ack_run(self.state, first["run_id"], self.full, 2, "auto")
        old_checkpoint = g.State(self.state).checkpoint("rutor")
        self.rutor_path.write_bytes(b"not raw deflate")
        report = self.run_once()
        self.assertEqual(report["status"], "degraded")
        self.assertEqual(report["sources"]["rutor"]["status"], "failed")
        self.assertEqual(report["counts"]["full"], 2)
        self.assertEqual(g.State(self.state).checkpoint("rutor"), old_checkpoint)
        g.ack_run(self.state, report["run_id"], self.full, 0, "delta")
        self.assertEqual(g.State(self.state).checkpoint("rutor"), old_checkpoint)

    def test_manual_full_replace_persists_omission_and_override(self):
        first = self.run_once()
        g.ack_run(self.state, first["run_id"], self.full, 2, "auto")
        second = self.run_once()
        rows = self.read_rows(self.full)
        edited = [row for row in rows if row["ih"] == "a" * 40]
        edited[0]["title"] = "Operator reviewed title"
        self.full.write_text("".join(json.dumps(row, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n" for row in edited), encoding="utf-8")
        ack = g.ack_run(self.state, second["run_id"], self.full, 1, "auto")
        self.assertEqual(ack["mode"], "replace")
        third = self.run_once()
        current = self.read_rows(self.full)
        self.assertEqual(len(current), 1)
        self.assertEqual(current[0]["title"], "Operator reviewed title")
        self.assertEqual(third["counts"]["delta"], 0)
        g.ack_run(self.state, third["run_id"], self.full, 0, "auto")

    def test_ack_count_mismatch_does_not_advance_checkpoint(self):
        report = self.run_once()
        with self.assertRaises(g.GrabberError):
            g.ack_run(self.state, report["run_id"], self.full, 99, "auto")
        state = g.State(self.state)
        self.assertIsNone(state.checkpoint("torrents-csv-data"))
        state.close()

    def test_tampered_delta_is_rejected_before_checkpoint_ack(self):
        report = self.run_once()
        original = self.delta.read_bytes()
        self.delta.write_bytes(original + b"\n")
        with self.assertRaisesRegex(g.GrabberError, "delta file was edited independently"):
            g.ack_run(self.state, report["run_id"], self.full, 2, "auto")
        state = g.State(self.state)
        try:
            self.assertIsNone(state.checkpoint("torrents-csv-data"))
        finally:
            state.close()
        self.delta.write_bytes(original)
        g.ack_run(self.state, report["run_id"], self.full, 2, "auto")

    def test_manual_clear_is_blocked_while_review_is_pending(self):
        report = self.run_once()
        with self.assertRaisesRegex(g.GrabberError, "resolve pending run"):
            g.manual_state(self.state, "clear", "both", "a" * 40)
        g.discard_run(self.state, report["run_id"])

    def test_all_sources_disabled_is_noop_without_pending_run(self):
        self.write_profile(csv_enabled=False, rutor_enabled=False)
        report = self.run_once()
        self.assertEqual(report["status"], "no-op")
        self.assertFalse((self.root / "work" / "pending-run.json").exists())

    def test_same_source_conflicting_duplicate_hash_is_quarantined(self):
        self.write_profile(rutor_enabled=False)
        with self.csv_path.open("a", encoding="utf-8") as stream:
            stream.write("a" * 40 + ',"Conflicting duplicate.mkv",1000,1700000000,0,0,0,0,0\n')
        report = self.run_once()
        rows = self.read_rows(self.full)
        self.assertNotIn("a" * 40, {row["ih"] for row in rows})
        self.assertGreater(report["sources"]["torrents-csv-data"]["conflicts"], 0)


    def test_volatile_csv_metrics_update_local_provenance_not_catalog_delta(self):
        self.write_profile(rutor_enabled=False)
        first = self.run_once()
        g.ack_run(self.state, first["run_id"], self.full, 1, "auto")
        original = self.csv_path.read_text(encoding="utf-8")
        self.csv_path.write_text(original.replace(",1000,1700000000,2,1,3,", ",1000,1700000000,99,7,123,"), encoding="utf-8")
        second = self.run_once()
        self.assertEqual(second["counts"]["delta"], 0)
        g.ack_run(self.state, second["run_id"], self.full, 0, "auto")
        state = g.State(self.state)
        try:
            row = state.db.execute("SELECT provenance_json FROM observations WHERE source_id=? AND ih=?",
                                   ("torrents-csv-data", "a" * 40)).fetchone()
            provenance = json.loads(row[0])
            self.assertEqual(provenance["seeders"], 99)
            self.assertEqual(provenance["leechers"], 7)
            self.assertEqual(provenance["completed"], 123)
        finally:
            state.close()

    def test_csv_partial_parse_is_rolled_back_when_source_fails(self):
        # The first CSV row is valid, but decoding fails later. The source savepoint
        # must discard the partial batch while the healthy Rutor adapter continues.
        self.rutor_json = [self.rutor_json[0]]
        self.write_rutor()
        original = self.csv_path.read_bytes()
        self.csv_path.write_bytes(original + b"\xff\xfe\xfa")
        report = self.run_once()
        self.assertEqual(report["sources"]["torrents-csv-data"]["status"], "failed")
        self.assertEqual(report["sources"]["rutor"]["status"], "success")
        rows = self.read_rows(self.full)
        self.assertNotIn("a" * 40, {row["ih"] for row in rows})
        self.assertIn("c" * 40, {row["ih"] for row in rows})

    def test_priority_and_category_mapping_changes_apply_to_cached_observations(self):
        first = self.run_once()
        g.ack_run(self.state, first["run_id"], self.full, 2, "auto")
        config = json.loads(self.profile.read_text(encoding="utf-8"))
        config["publisher"]["category_map"]["Movies"] = "films"
        config["publisher"]["shard_policy"]["category_map"] = {"films": "movie-shard"}
        for source in config["sources"]:
            if source["id"] == "rutor":
                source["priority"] = 5
        self.profile.write_text(json.dumps(config), encoding="utf-8")
        second = self.run_once()
        rows = {row["ih"]: row for row in self.read_rows(self.full)}
        # Existing published shard stays stable, while category tags are re-mapped.
        self.assertEqual(rows["c" * 40]["tags"], ["films"])
        self.assertEqual(rows["c" * 40]["shard"], "films")
        g.ack_run(self.state, second["run_id"], self.full, second["counts"]["delta"], "delta")
        state = g.State(self.state)
        try:
            priority = state.db.execute("SELECT priority FROM observations WHERE source_id='rutor' AND ih=?", ("c" * 40,)).fetchone()[0]
            self.assertEqual(priority, 5)
        finally:
            state.close()

    def test_manual_replace_suppresses_newly_discovered_unstaged_rows(self):
        self.write_profile(rutor_enabled=False)
        first = self.run_once()
        g.ack_run(self.state, first["run_id"], self.full, 1, "auto")
        with self.csv_path.open("a", encoding="utf-8") as stream:
            stream.write("d" * 40 + ',"New item.mkv",3000,1700000200,0,0,0,0,0\n')
        second = self.run_once()
        self.assertEqual(second["counts"]["full"], 2)
        # The new row has never been published. Omitting it from the edited full
        # file must still persist a suppression before the source checkpoint advances.
        only_old = self.read_rows(self.full)[0]
        self.full.write_text(json.dumps(only_old, separators=(",", ":")) + "\n", encoding="utf-8")
        ack = g.ack_run(self.state, second["run_id"], self.full, 1, "auto")
        self.assertEqual(ack["mode"], "replace")
        third = self.run_once()
        self.assertEqual(third["counts"]["full"], 1)
        self.assertNotIn("d" * 40, {row["ih"] for row in self.read_rows(self.full)})
        g.ack_run(self.state, third["run_id"], self.full, 0, "auto")


    def test_safe_tracker_filter_drops_private_announce_credentials(self):
        self.assertEqual(g._safe_tracker_url("udp://tracker.example:80/announce"),
                         "udp://tracker.example:80/announce")
        self.assertIsNone(g._safe_tracker_url("https://tracker.example/announce?passkey=secret"))
        self.assertIsNone(g._safe_tracker_url("https://tracker.example/privatepasskey1234567890123456/announce"))
        self.assertIsNone(g._safe_tracker_url("https://user:secret@tracker.example/announce"))
        self.assertIsNone(g._safe_tracker_url("udp://tracker.example:99999/announce"))

    def test_manual_list_and_clear_operations(self):
        report = self.run_once()
        g.ack_run(self.state, report["run_id"], self.full, 2, "auto")
        next_run = self.run_once()
        edited = self.read_rows(self.full)
        edited[0]["title"] = "Manual override"
        self.full.write_text("".join(json.dumps(row, separators=(",", ":")) + "\n" for row in edited[:1]), encoding="utf-8")
        g.ack_run(self.state, next_run["run_id"], self.full, 1, "auto")
        listed = g.manual_state(self.state, "list")
        self.assertIn("a" * 40, {row["ih"] for row in listed["overrides"]})
        self.assertIn("c" * 40, {row["ih"] for row in listed["suppressions"]})
        cleared = g.manual_state(self.state, "clear", "both", "a" * 40)
        self.assertEqual(cleared["removed"]["overrides"], 1)
        g.manual_state(self.state, "clear", "suppression", "c" * 40)
        rerun = self.run_once()
        values = {row["ih"]: row for row in self.read_rows(self.full)}
        self.assertEqual(len(values), 2)
        self.assertEqual(values["a" * 40]["title"], "Higher priority title")
        self.assertIn("c" * 40, values)
        g.discard_run(self.state, rerun["run_id"])

    def test_binary_and_decimal_size_rounding(self):
        self.assertEqual(g._parse_size("1.5 MB", "binary"), 1572864)
        self.assertEqual(g._parse_size("1.5 MB", "decimal"), 1500000)
        self.assertEqual(g._parse_size("1.0000005 B", "decimal"), 1)



    def test_rutracker_xz_snapshot_tombstone_and_provenance(self):
        self.write_profile(rutor_enabled=False)
        first = self.run_once()
        g.ack_run(self.state, first["run_id"], self.full, 1, "auto")
        config = json.loads(self.profile.read_text(encoding="utf-8"))
        config["sources"].append({
            "id": "rutracker", "type": "rutracker", "enabled": True, "priority": 30,
            "path": str(self.rutracker_path), "forum_include_ids": ["100"],
            "max_topics": 3
        })
        self.profile.write_text(json.dumps(config), encoding="utf-8")
        # This topic has no nested torrent/hash. It must be discarded by forum ID
        # before detailed parsing, rather than quarantined as a malformed record.
        filtered_topic = (
            b'  <torrent id="filtered-topic"><title>Ignored music item</title>'
            b'<forum id="200">Music</forum></torrent>\n'
        )
        filtered_xml = self.rutracker_xml.replace(
            b"</rutracker>", filtered_topic + b"</rutracker>")
        self.rutracker_path.write_bytes(lzma.compress(filtered_xml))
        second = self.run_once()
        self.assertEqual(second["sources"]["rutracker"]["status"], "success")
        self.assertEqual(second["sources"]["rutracker"]["fetched"], 3)
        self.assertEqual(second["sources"]["rutracker"]["filtered"], 1)
        self.assertEqual(second["sources"]["rutracker"]["rejected"], 0)
        self.assertEqual(second["sources"]["rutracker"]["topic_limit"], 3)
        self.assertEqual(second["sources"]["rutracker"]["checkpoint"]["compression"], "xz")
        full, delta = self.read_rows(self.full), self.read_rows(self.delta)
        self.assertEqual({row["ih"] for row in full}, {"d" * 40})
        self.assertIn({"del": "a" * 40, "shard": "default"}, delta)
        row = full[0]
        self.assertEqual(row["title"], "RuTracker new release")
        self.assertEqual(row["date"], 1704276672)
        self.assertEqual(row["files"], 1)
        self.assertEqual(row["trackers"], ["http://bt2.t-ru.org/ann?magnet"])
        self.assertEqual(row["tags"], ["movie"])
        g.ack_run(self.state, second["run_id"], self.full, len(delta), "auto")
        state = g.State(self.state)
        try:
            saved = state.db.execute(
                "SELECT provenance_json FROM observations WHERE source_id='rutracker' AND ih=?",
                ("d" * 40,)).fetchone()
            provenance = json.loads(saved[0])
            self.assertEqual(provenance["old"][0]["hash"], "c" * 40)
            self.assertEqual(provenance["dup_hints"][0]["p"], "similar")
            self.assertEqual(provenance["content_text"], "Sanitized description")
        finally:
            state.close()
        # A newer full XML snapshot still carries the acknowledged historical <del/>.
        # It must not be treated as an unknown deletion or emitted twice.
        updated_xml = self.rutracker_xml.replace(b"RuTracker new release", b"Updated RuTracker release")
        self.rutracker_path.write_bytes(lzma.compress(updated_xml))
        third = self.run_once()
        self.assertEqual(third["sources"]["rutracker"]["status"], "success")
        self.assertEqual(third["sources"]["rutracker"]["rejected"], 0)
        self.assertEqual(third["counts"]["delta"], 1)
        self.assertFalse(any("del" in row for row in self.read_rows(self.delta)))
        self.assertEqual(self.read_rows(self.full)[0]["title"], "Updated RuTracker release")
        g.ack_run(self.state, third["run_id"], self.full, 1, "auto")
        fourth = self.run_once()
        self.assertEqual(fourth["counts"]["delta"], 0)
        g.ack_run(self.state, fourth["run_id"], self.full, 0, "auto")

    def test_rutracker_large_but_bounded_profile_limits(self):
        config = {
            "version": 1,
            "sources": [{
                "id": "rutracker", "type": "rutracker", "enabled": True,
                "path": str(self.rutracker_path), "priority": 30,
                "max_compressed_bytes": 6666666666,
                "max_expanded_bytes": 66666666666,
                "xz_memory_limit": 1152 * 1024 * 1024,
                "max_topics": 10000000,
            }],
        }
        self.profile.write_text(json.dumps(config), encoding="utf-8")
        loaded = g._load_config(self.profile)["sources"][0]
        self.assertEqual(loaded["max_expanded_bytes"], 66666666666)
        self.assertEqual(loaded["xz_memory_limit"], 1152 * 1024 * 1024)

        config["sources"][0]["xz_memory_limit"] = g.MAX_RUTRACKER_XZ_MEMORY_BYTES + 1
        self.profile.write_text(json.dumps(config), encoding="utf-8")
        with self.assertRaisesRegex(g.GrabberError, "xz_memory_limit"):
            g._load_config(self.profile)

        config["sources"][0]["xz_memory_limit"] = 1152 * 1024 * 1024
        config["sources"][0]["max_compressed_bytes"] = g.MAX_RUTRACKER_COMPRESSED_BYTES + 1
        self.profile.write_text(json.dumps(config), encoding="utf-8")
        with self.assertRaisesRegex(g.GrabberError, "max_compressed_bytes"):
            g._load_config(self.profile)

    def test_rutracker_topic_scan_limit_is_separate_from_catalog_limit(self):
        self.write_profile(csv_enabled=False, rutor_enabled=False)
        config = json.loads(self.profile.read_text(encoding="utf-8"))
        config["sources"].append({
            "id": "rutracker", "type": "rutracker", "enabled": True,
            "priority": 30, "path": str(self.rutracker_path), "max_topics": 1,
        })
        self.profile.write_text(json.dumps(config), encoding="utf-8")
        report = self.run_once()
        source_report = report["sources"]["rutracker"]
        self.assertEqual(source_report["status"], "failed")
        self.assertIn("max_topics", source_report["reason"])

    def test_rutracker_delete_without_published_shard_is_quarantined(self):
        config = {"version": 1, "publisher": {}, "sources": [
            {"id": "rutracker", "type": "rutracker", "enabled": True,
             "priority": 30, "path": str(self.rutracker_path)}]}
        self.profile.write_text(json.dumps(config), encoding="utf-8")
        report = self.run_once()
        self.assertGreater(report["sources"]["rutracker"]["rejected"], 0)
        self.assertGreater(report["sources"]["rutracker"]["quarantined"], 0)
        self.assertNotIn("a" * 40, {row["ih"] for row in self.read_rows(self.full)})
        self.assertIn("d" * 40, {row["ih"] for row in self.read_rows(self.full)})
        g.discard_run(self.state, report["run_id"])

    def test_rutracker_xz_concatenated_streams_and_padding(self):
        first = b"<root><item>first</item>"
        second = b"<item>second</item></root>"
        payload = (
            lzma.compress(first)
            + (b"\x00" * 4)
            + lzma.compress(second)
            + (b"\x00" * 8)
        )
        low_memory = ga._XzReader(io.BytesIO(lzma.compress(b"memory test", preset=9)),
                                  16 * 1024 * 1024)
        with self.assertRaisesRegex(g.SourceError, "xz_memory_limit"):
            while low_memory.read(8):
                pass

        reader = ga._XzReader(io.BytesIO(payload), 128 * 1024 * 1024)
        chunks = []
        while True:
            chunk = reader.read(17)
            if not chunk:
                break
            chunks.append(chunk)
        self.assertEqual(b"".join(chunks), first + second)
        self.assertEqual(reader.streams, 2)

        invalid_padding = ga._XzReader(
            io.BytesIO(lzma.compress(b"ok") + b"\x00\x00"),
            128 * 1024 * 1024,
        )
        with self.assertRaisesRegex(g.SourceError, "invalid stream padding"):
            while invalid_padding.read(8):
                pass

        trailing_junk = ga._XzReader(
            io.BytesIO(lzma.compress(b"ok") + b"NOT-XZ"),
            128 * 1024 * 1024,
        )
        with self.assertRaisesRegex(g.SourceError, "trailing data"):
            while trailing_junk.read(8):
                pass

    def test_rutracker_rejects_corrupt_xz_and_xml_entities(self):
        self.rutracker_path.write_bytes(b"\xfd7zXZ\x00broken")
        config = {"version": 1, "publisher": {}, "sources": [
            {"id": "rutracker", "type": "rutracker", "enabled": True, "path": str(self.rutracker_path)}]}
        self.profile.write_text(json.dumps(config), encoding="utf-8")
        report = self.run_once()
        self.assertEqual(report["sources"]["rutracker"]["status"], "failed")
        self.assertFalse((self.root / "work" / "pending-run.json").exists())
        malicious = self.root / "evil.xml"
        malicious.write_text('<!DOCTYPE x [<!ENTITY boom "boom">]><x/>', encoding="utf-8")
        raw = malicious.open("rb")
        try:
            bounded = ga._BoundedXmlReader(raw, 1024)
            iterator = __import__("xml.etree.ElementTree", fromlist=["iterparse"]).iterparse(bounded)
            with self.assertRaisesRegex(g.SourceError, "DTD and entity"):
                list(iterator)
        finally:
            raw.close()

    def test_torznab_caps_recent_items_and_overlap_replay(self):
        caps, feed = self.torznab_caps, self.torznab_feed
        feed_state = {"payload": feed}
        requests = []
        class Handler(BaseHTTPRequestHandler):
            def do_GET(handler):
                query = urllib.parse.parse_qs(urllib.parse.urlsplit(handler.path).query)
                requests.append(query)
                if query.get("apikey") != ["unit-test-key"]:
                    handler.send_response(403); handler.end_headers(); return
                payload = caps if query.get("t") == ["caps"] else feed_state["payload"]
                handler.send_response(200)
                handler.send_header("Content-Type", "application/xml")
                handler.send_header("Content-Length", str(len(payload)))
                handler.end_headers()
                handler.wfile.write(payload)
            def log_message(self, fmt, *args):
                return
        server = self.start_http_server(Handler)
        config = {
            "version": 1,
            "publisher": {"category_map": {"Movies HD": "film"},
                          "shard_policy": {"default": "default", "category_map": {"film": "films"}}},
            "sources": [{"id": "jackett-indexer-a", "type": "jackett-torznab", "enabled": True,
                         "priority": 40, "url": f"http://127.0.0.1:{server.server_port}/api/torznab",
                         "api_key": "unit-test-key", "timeout": 5, "page_size": 25,
                         "max_pages": 2, "max_items": 100, "overlap_seconds": 86400}]
        }
        self.profile.write_text(json.dumps(config), encoding="utf-8")
        self.assertEqual(ga._torznab_caps(caps)["2010"], "Movies HD")
        first = self.run_once()
        source = first["sources"]["jackett-indexer-a"]
        self.assertEqual(source["accepted"], 1)
        self.assertEqual(source["rejected"], 1)
        rows = self.read_rows(self.full)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["ih"], "e" * 40)
        self.assertEqual(rows[0]["tags"], ["film"])
        self.assertEqual(rows[0]["shard"], "films")
        self.assertNotIn("unit-test-key", self.report.read_text(encoding="utf-8"))
        self.assertTrue(any(q.get("t") == ["caps"] for q in requests))
        self.assertTrue(any(q.get("t") == ["search"] for q in requests))
        g.ack_run(self.state, first["run_id"], self.full, 1, "auto")

        # Delayed item within the overlap must be ingested; stale item outside it
        # must be filtered. Both arrive after the original poll checkpoint.
        late_item = (
            f"<item><title>Delayed release</title><guid>delayed-guid</guid>"
            f"<pubDate>Wed, 03 Jan 2024 09:30:00 +0000</pubDate>"
            f"<enclosure url='magnet:?xt=urn:btih:{'f'*40}' length='222' type='application/x-bittorrent'/>"
            f"</item>"
        )
        stale_item = (
            f"<item><title>Too old release</title><guid>stale-guid</guid>"
            f"<pubDate>Fri, 01 Dec 2023 09:30:00 +0000</pubDate>"
            f"<enclosure url='magnet:?xt=urn:btih:{'1'*40}' length='333' type='application/x-bittorrent'/>"
            f"</item>"
        )
        feed_state["payload"] = feed.replace(b"</channel>", (late_item + stale_item).encode() + b"</channel>")
        second = self.run_once()
        self.assertEqual(second["sources"]["jackett-indexer-a"]["accepted"], 1)
        self.assertGreaterEqual(second["sources"]["jackett-indexer-a"]["duplicates"], 1)
        self.assertGreaterEqual(second["sources"]["jackett-indexer-a"]["filtered"], 1)
        self.assertEqual(second["counts"]["delta"], 1)
        full_hashes = {row["ih"] for row in self.read_rows(self.full)}
        self.assertEqual(full_hashes, {"e" * 40, "f" * 40})
        self.assertNotIn("1" * 40, full_hashes)
        g.ack_run(self.state, second["run_id"], self.full, 1, "auto")

        third = self.run_once()
        self.assertEqual(third["sources"]["jackett-indexer-a"]["accepted"], 0)
        self.assertGreaterEqual(third["sources"]["jackett-indexer-a"]["duplicates"], 2)
        self.assertEqual(third["counts"]["delta"], 0)
        g.ack_run(self.state, third["run_id"], self.full, 0, "auto")

    def test_bad_indexer_does_not_block_next_indexer(self):
        caps, feed = self.torznab_caps, self.torznab_feed
        class Handler(BaseHTTPRequestHandler):
            def do_GET(handler):
                query = urllib.parse.parse_qs(urllib.parse.urlsplit(handler.path).query)
                payload = caps if query.get("t") == ["caps"] else feed
                handler.send_response(200)
                handler.send_header("Content-Length", str(len(payload)))
                handler.end_headers()
                handler.wfile.write(payload)
            def log_message(self, fmt, *args): return
        server = self.start_http_server(Handler)
        config = {"version": 1, "publisher": {}, "sources": [
            {"id": "broken-indexer", "type": "torznab", "enabled": True,
             "priority": 40, "url": "http://127.0.0.1:1/api", "timeout": 1},
            {"id": "healthy-indexer", "type": "torznab", "enabled": True,
             "priority": 50, "url": f"http://127.0.0.1:{server.server_port}/api", "timeout": 5}
        ]}
        self.profile.write_text(json.dumps(config), encoding="utf-8")
        report = self.run_once()
        self.assertEqual(report["sources"]["broken-indexer"]["status"], "failed")
        self.assertEqual(report["sources"]["healthy-indexer"]["accepted"], 1)
        self.assertIn("e" * 40, {row["ih"] for row in self.read_rows(self.full)})
        g.discard_run(self.state, report["run_id"])

    def test_html_selector_validation_accepts_tags_and_rejects_invalid_attributes(self):
        for selector in ("h2", "span", "a.magnet", "article.release", "time[datetime]",
                         "span[data-bytes]", "div.result h2", "div.result > h2"):
            # Child combinators are intentionally not supported by this small selector grammar.
            if ">" in selector:
                with self.assertRaises(gt.TemplateError):
                    gt._parse_selector(selector)
            else:
                self.assertTrue(gt._parse_selector(selector))
        with self.assertRaises(gt.TemplateError):
            gt._parse_selector("span[=broken]")
        with self.assertRaises(gt.TemplateError):
            gt._parse_selector("span[data-bytes=]")

    def test_json_html_and_text_template_formats(self):
        json_template = {"format": "json", "record_path": "$.results[*]",
            "fields": {"ih": "$.hash", "title": "$.name", "date": "$.created",
                       "size": "$.bytes", "categories": {"path": "$.categories", "multiple": True}}}
        parsed = gt.parse_template(
            b'{"results":[{"hash":"' + b"f" * 40 + b'","name":"JSON release","created":1700000000,"bytes":123,"categories":["Movies"]}]}',
            json_template)
        self.assertEqual(parsed[0]["ih"], "f" * 40)
        self.assertEqual(parsed[0]["categories"], ["Movies"])
        html_template = {"format": "html", "item_selector": "div.result",
            "fields": {"magnet": {"selector": "a.magnet", "attr": "href"},
                       "title": {"selector": "h2"}, "size": {"selector": "span.size", "attr": "data-bytes"}}}
        html_rows = gt.parse_template(
            (f'<div class="result"><h2>HTML release</h2><a class="magnet" href="magnet:?xt=urn:btih:{"1"*40}">get</a><span class="size" data-bytes="456"></span></div>').encode(),
            html_template)
        self.assertEqual(html_rows[0]["title"], "HTML release")
        ordered_html = gt.parse_template(
            b'<div class="result"><h2>Before <b>middle</b> after</h2></div>',
            {"format": "html", "item_selector": "div.result",
             "fields": {"title": {"selector": "h2"}}})
        self.assertEqual(ordered_html[0]["title"], "Before middle after")
        text_template = {"format": "text",
            "pattern": r"^(?P<ih>[0-9a-f]{40})\|(?P<title>[^|]+)\|(?P<date>[0-9]+)$",
            "fields": {"ih": {"group": "ih"}, "title": {"group": "title"}, "date": {"group": "date"}}}
        rows = gt.parse_template((("2" * 40) + "|Text release|1700000100\n").encode(), text_template)
        self.assertEqual(rows[0]["title"], "Text release")
        self.assertEqual(rows[0]["date"], "1700000100")

    def test_priority_aware_tombstones_and_restoration(self):
        ih = "a" * 40
        def row(priority, record):
            return {"priority": priority, "record_json": json.dumps(record),
                    "categories_json": "[]"}
        deleted, conflict = g._merge_observations([
            row(10, {"ih": ih, "title": "Old title"}),
            row(20, {"del": ih, "shard": "films"}),
        ], {})
        self.assertIsNone(conflict)
        self.assertEqual(deleted, {"del": ih, "shard": "films"})
        restored, conflict = g._merge_observations([
            row(10, {"ih": ih, "title": "Old title"}),
            row(20, {"del": ih, "shard": "films"}),
            row(30, {"ih": ih, "title": "Restored title", "shard": "films"}),
        ], {})
        self.assertIsNone(conflict)
        self.assertEqual(restored["title"], "Restored title")
        self.assertEqual(restored["ih"], ih)
        _, conflict = g._merge_observations([
            row(20, {"del": ih, "shard": "films"}),
            row(20, {"ih": ih, "title": "Conflicting live row"}),
        ], {})
        self.assertIn("same-priority live/tombstone conflict", conflict)

    def test_checked_in_example_profile_validates(self):
        profile_path = Path(__file__).resolve().parent / "grabber-profile.example.json"
        config = g._load_config(profile_path)
        self.assertIn("generic-json", config["templates"])
        self.assertEqual(config["templates"]["generic-text"]["format"], "text")
        self.assertTrue({"csv", "torrents-csv-data", "rutor", "rutracker",
                         "torznab", "jackett-torznab", "rss", "template"}.issubset(
                             ga.SOURCE_ADAPTERS))
        self.assertTrue(all(source.get("enabled") is False for source in config["sources"]))

    def test_direct_rss_uses_feed_parser_without_caps(self):
        feed = self.torznab_feed
        requests = []
        class Handler(BaseHTTPRequestHandler):
            def do_GET(handler):
                requests.append(handler.path)
                handler.send_response(200)
                handler.send_header("Content-Length", str(len(feed)))
                handler.end_headers()
                handler.wfile.write(feed)
            def log_message(self, fmt, *args): return
        server = self.start_http_server(Handler)
        config = {"version": 1, "publisher": {}, "sources": [
            {"id": "direct-rss-a", "type": "rss", "enabled": True, "priority": 50,
             "url": f"http://127.0.0.1:{server.server_port}/releases.rss",
             "fetch_caps": False, "timeout": 5, "max_items": 50,
             "overlap_seconds": 86400}
        ]}
        self.profile.write_text(json.dumps(config), encoding="utf-8")
        first = self.run_once()
        self.assertEqual(first["sources"]["direct-rss-a"]["accepted"], 1)
        self.assertEqual(first["sources"]["direct-rss-a"]["rejected"], 1)
        self.assertEqual(requests, ["/releases.rss"])
        g.ack_run(self.state, first["run_id"], self.full, 1, "auto")
        second = self.run_once()
        self.assertEqual(second["counts"]["delta"], 0)
        g.ack_run(self.state, second["run_id"], self.full, 0, "auto")

    def test_template_source_reads_profile_templates_and_acknowledges_checkpoint(self):
        payload = {"entries": [{"hash": "3" * 40, "name": "Configured template release",
                                "stamp": 1700000200, "bytes": "1.5 MB"}]}
        path = self.root / "custom-source.json"
        path.write_text(json.dumps(payload), encoding="utf-8")
        config = {"version": 1, "publisher": {},
            "templates": {"custom-json": {"format": "json", "record_path": "$.entries[*]",
                "fields": {"ih": "$.hash", "title": "$.name", "date": "$.stamp", "size": "$.bytes"}}},
            "sources": [{"id": "custom-source", "type": "template", "template": "custom-json",
                         "enabled": True, "priority": 60, "path": str(path), "size_unit": "binary"}]}
        self.profile.write_text(json.dumps(config), encoding="utf-8")
        first = self.run_once()
        self.assertEqual(first["sources"]["custom-source"]["accepted"], 1)
        self.assertEqual(self.read_rows(self.full)[0]["size"], 1572864)
        g.ack_run(self.state, first["run_id"], self.full, 1, "auto")
        second = self.run_once()
        self.assertEqual(second["sources"]["custom-source"]["status"], "unchanged")
        g.ack_run(self.state, second["run_id"], self.full, 0, "auto")


if __name__ == "__main__":
    unittest.main()
