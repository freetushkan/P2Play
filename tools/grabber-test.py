#!/usr/bin/env python3
"""Small, deterministic tests for the review-first catalog grabber."""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
import grabber_lib as g


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
        fixtures = Path(__file__).resolve().parent / "fixtures" / "grabber"
        self.csv_path.write_bytes((fixtures / "torrents.csv").read_bytes())
        subprocess.run(["git", "-C", str(self.repo), "add", "torrents.csv"], check=True)
        subprocess.run(["git", "-C", str(self.repo), "commit", "-m", "fixture"], check=True, capture_output=True)

        fixtures = Path(__file__).resolve().parent / "fixtures" / "grabber"
        self.rutor_json = json.loads((fixtures / "rutor.json").read_text(encoding="utf-8"))
        self.rutor_path = self.root / "rutor.ls"
        self.write_rutor()
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

    def read_rows(self, path):
        return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]

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


if __name__ == "__main__":
    unittest.main()
