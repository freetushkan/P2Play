#!/usr/bin/env python3
"""Measure a large MediaMesh list with two real p2playd processes.

Usage:
  python3 -I tools/mesh-scale-test.py /absolute/path/to/p2playd

The profile publishes a multi-segment catalog, transfers only one selected shard,
then fetches the maximum-size description and poster on demand. It reports disk,
loopback traffic, RSS and elapsed-time measurements; platform-sensitive timings
are reported rather than used as brittle pass/fail thresholds.
"""
import json
import os
from pathlib import Path
import runpy
import shutil
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

HELPERS = runpy.run_path(str(Path(__file__).with_name("peer-items-test.py")))
Node = HELPERS["Node"]
TOKEN = HELPERS["TOKEN"]

MIB = 1024 * 1024
DESCRIPTION_LIMIT = 64 * 1024
POSTER_LIMIT = 2 * MIB
RECORD_BUDGET = 64 * MIB
MEDIA_BUDGET = 8 * MIB
CACHE_BUDGET = 8 * MIB
MOVIE_RECORDS = 20_000
SERIES_RECORDS = 512


def check(condition, message):
    if not condition:
        raise AssertionError(message)
    print("OK   " + message, flush=True)


def wait_for(callback, seconds=150, interval=0.25, label="condition"):
    deadline = time.monotonic() + seconds
    last = None
    while time.monotonic() < deadline:
        try:
            last = callback()
            if last:
                return last
        except (OSError, urllib.error.URLError, KeyError, IndexError):
            pass
        time.sleep(interval)
    raise AssertionError(f"timed out waiting for {label}; last value: {last!r}")


def post_raw(node, path, data, content_type):
    request = urllib.request.Request(
        f"http://127.0.0.1:{node.http}{path}",
        data=data,
        headers={"Authorization": "Bearer " + TOKEN, "Content-Type": content_type},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=90) as response:
            return response.status, response.read(), response.headers
    except urllib.error.HTTPError as error:
        return error.code, error.read(), error.headers


def get_raw(node, path):
    request = urllib.request.Request(
        f"http://127.0.0.1:{node.http}{path}",
        headers={"Authorization": "Bearer " + TOKEN},
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return response.status, response.read(), response.headers
    except urllib.error.HTTPError as error:
        return error.code, error.read(), error.headers


def tree_bytes(path):
    path = Path(path)
    if not path.exists():
        return 0
    total = 0
    for current, _dirs, files in os.walk(path, followlinks=False):
        for name in files:
            item = Path(current) / name
            try:
                if not item.is_symlink():
                    total += item.stat().st_size
            except FileNotFoundError:
                pass
    return total


def tree_file_count(path):
    path = Path(path)
    if not path.exists():
        return 0
    return sum(
        1 for current, _dirs, files in os.walk(path, followlinks=False)
        for name in files
        if not (Path(current) / name).is_symlink()
    )


def loopback_counters():
    root = Path("/sys/class/net/lo/statistics")
    return {
        "rx": int((root / "rx_bytes").read_text()),
        "tx": int((root / "tx_bytes").read_text()),
    }


def proc_memory(pid):
    values = {}
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if line.startswith(("VmRSS:", "VmHWM:")):
            key, value, *_ = line.split()
            values[key.rstrip(":")] = int(value) * 1024
    return values


def build_records(description_ref, poster_ref):
    lines = []
    for i in range(MOVIE_RECORDS):
        # Near-limit titles make the normalized NDJSON cross the publisher's
        # 8 MiB split boundary without duplicating two media references in
        # every record. One representative record exercises both media refs.
        record = {
            "ih": f"{i + 1:040x}",
            "title": f"Catalog Feature {i:06d} " + ("x" * 300),
            "shard": "movies",
            "size": 1_400_000_000 + i,
            "files": 1,
            "date": 1_760_000_000 - i,
            "tags": ["benchmark", "feature"],
        }
        if i == 0:
            record["description_ref"] = description_ref
            record["poster_ref"] = poster_ref
        lines.append(json.dumps(record, ensure_ascii=False, separators=(",", ":")))
    for i in range(SERIES_RECORDS):
        record = {
            "ih": f"{MOVIE_RECORDS + i + 1:040x}",
            "title": f"Catalog Series {i:05d}",
            "shard": "series",
            "size": 850_000_000 + i,
            "files": 12,
            "date": 1_750_000_000 - i,
            "tags": ["benchmark", "series"],
        }
        lines.append(json.dumps(record, ensure_ascii=False, separators=(",", ":")))
    return ("\n".join(lines) + "\n").encode("utf-8")


def metrics_summary(metrics):
    return (
        "## MediaMesh large-catalog profile\n\n"
        "| Measurement | Result |\n|---|---:|\n"
        + "".join(f"| {key.replace('_', ' ')} | {value} |\n"
                  for key, value in metrics.items())
    )


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: mesh-scale-test.py /absolute/path/to/p2playd")
    daemon = str(Path(sys.argv[1]).resolve())
    if not Path(daemon).is_file():
        raise SystemExit("p2playd executable not found")

    root = tempfile.mkdtemp(prefix="p2play-mesh-scale-")
    nodes = []
    metrics = {}
    try:
        publisher = Node(daemon, root, "publisher")
        nodes.append(publisher)
        subscriber = Node(daemon, root, "subscriber")
        nodes.append(subscriber)

        status, created = publisher.call(
            "/api/publish", {"name": "Large catalog resource profile",
                             "description": "Synthetic acceptance profile"}
        )
        check(status == 201 and len(created.get("pubkey", "")) == 64,
              "publisher list created")
        pubkey = created["pubkey"]

        description = b"D" * DESCRIPTION_LIMIT
        poster = b"\xff\xd8\xff" + b"P" * (POSTER_LIMIT - 5) + b"\xff\xd9"
        status, body, _ = post_raw(
            publisher, f"/api/publish/{pubkey}/media/description",
            description, "text/plain; charset=utf-8"
        )
        check(status == 201, "64 KiB UTF-8 description accepted")
        description_ref = json.loads(body)
        status, body, _ = post_raw(
            publisher, f"/api/publish/{pubkey}/media/poster",
            poster, "image/jpeg"
        )
        check(status == 201, "2 MiB poster accepted")
        poster_ref = json.loads(body)

        status, _body, _ = post_raw(
            publisher, f"/api/publish/{pubkey}/media/description",
            description + b"x", "text/plain; charset=utf-8"
        )
        check(status == 400, "description above 64 KiB refused")
        status, _body, _ = post_raw(
            publisher, f"/api/publish/{pubkey}/media/poster",
            poster + b"x", "image/jpeg"
        )
        check(status == 400, "poster above 2 MiB refused")

        ndjson = build_records(description_ref, poster_ref)
        check(8 * MIB < len(ndjson) < 15 * MIB,
              f"catalog payload exercises the 8 MiB segment split ({len(ndjson)} bytes)")
        metrics["record_upload_bytes"] = len(ndjson)
        metrics["movie_records"] = MOVIE_RECORDS
        metrics["unselected_series_records"] = SERIES_RECORDS
        metrics["description_bytes"] = len(description)
        metrics["poster_bytes"] = len(poster)

        started = time.monotonic()
        status, accepted, _ = post_raw(
            publisher, f"/api/publish/{pubkey}/records",
            ndjson, "application/x-ndjson"
        )
        check(status == 200 and json.loads(accepted)["count"] == MOVIE_RECORDS + SERIES_RECORDS,
              "all large-catalog records accepted")
        status, released = publisher.call(f"/api/publish/{pubkey}/release", {})
        check(status == 200 and released["records"] == MOVIE_RECORDS + SERIES_RECORDS,
              "multi-shard version released")
        metrics["publish_seconds"] = round(time.monotonic() - started, 3)

        wait_for(
            lambda: publisher.call("/api/lists")[1]["lists"][0]["net"]["seeding"],
            seconds=30, label="publisher torrent seeding"
        )
        status, publisher_plan = publisher.call(f"/api/lists/{pubkey}/files")
        check(status == 200, "publisher signed file plan available")
        publisher_files = publisher_plan["files"]
        movie_segments = [f for f in publisher_files
                          if f["kind"] == "records" and f["shard"] == "movies"]
        series_segments = [f for f in publisher_files
                           if f["kind"] == "records" and f["shard"] == "series"]
        media_packs = [f for f in publisher_files if f["kind"] in ("description", "poster")]
        check(len(movie_segments) >= 2,
              f"movie shard split into {len(movie_segments)} immutable record segments")
        check(all(0 < f["bytes"] <= 64 * MIB for f in movie_segments + series_segments),
              "all published record segments remain within the 64 MiB limit")
        check(len(media_packs) == 2 and all(f["bytes"] <= 16 * MIB for f in media_packs),
              "description and poster are held in bounded, separate media packs")
        publisher_dir = Path(publisher.data) / "lists" / pubkey
        metrics["publisher_list_disk_bytes"] = tree_bytes(publisher_dir)
        metrics["publisher_list_file_count"] = tree_file_count(publisher_dir)
        metrics["movie_segment_count"] = len(movie_segments)
        metrics["movie_segment_compressed_bytes"] = sum(f["bytes"] for f in movie_segments)
        metrics["series_segment_count"] = len(series_segments)
        metrics["media_pack_bytes"] = sum(f["bytes"] for f in media_packs)

        status, policy = subscriber.call(
            f"/api/lists/{pubkey}/policy",
            {"shards": ["movies"], "record_bytes": RECORD_BUDGET,
             "media_bytes": MEDIA_BUDGET, "cache_bytes": CACHE_BUDGET,
             "mirror_media": False}
        )
        check(status == 200, "subscriber resource policy configured")
        before_subscribe = loopback_counters()
        started = time.monotonic()
        status, response = subscriber.call(
            f"/api/lists/{pubkey}", {"peers": [f"127.0.0.1:{publisher.bt}"]}
        )
        check(status == 202, f"subscriber accepted peer-hinted subscription ({response})")

        def ready():
            for item in subscriber.lists():
                if item["pubkey"] == pubkey and item["seq"] == 1 and item["net"].get("records_ready"):
                    return item
            return None

        received = wait_for(ready, seconds=150, label="selected movie shard transfer")
        metrics["subscribe_seconds"] = round(time.monotonic() - started, 3)
        after_subscribe = loopback_counters()
        metrics["initial_loopback_rx_bytes"] = after_subscribe["rx"] - before_subscribe["rx"]
        metrics["initial_loopback_tx_bytes"] = after_subscribe["tx"] - before_subscribe["tx"]
        check(received["records"] == MOVIE_RECORDS,
              f"subscriber indexed exactly {MOVIE_RECORDS} selected records")
        check(subscriber.call(f"/api/catalog/{MOVIE_RECORDS + 1:040x}")[0] == 404,
              "unselected series shard is absent from the subscriber index")

        status, subscriber_plan = subscriber.call(f"/api/lists/{pubkey}/files")
        check(status == 200, "subscriber signed file plan available")
        received_files = subscriber_plan["files"]
        selected = [f for f in received_files if f["kind"] == "records" and f["shard"] == "movies"]
        unselected = [f for f in received_files if f["kind"] == "records" and f["shard"] == "series"]
        check(selected and all(f["selected"] and f["present"] for f in selected),
              "all selected movie segments are present")
        check(unselected and all(not f["selected"] and not f["present"] for f in unselected),
              "unselected series segments are not downloaded")
        subscriber_media_dir = Path(subscriber.data) / "lists" / pubkey / "media"
        check(not list(subscriber_media_dir.glob("*.bin")),
              "description/poster packs are not eagerly retained")

        query = urllib.parse.quote("Catalog Feature 01234")
        started = time.monotonic()
        status, result = subscriber.call(f"/api/catalog/search?q={query}&limit=5")
        metrics["search_seconds"] = round(time.monotonic() - started, 4)
        check(status == 200 and any(r["hash"] == f"{1235:040x}" for r in result["results"]),
              "large catalog is searchable by indexed title")
        selected_record_bytes = sum(f["bytes"] for f in selected)
        check(selected_record_bytes <= RECORD_BUDGET,
              "selected record segments fit the configured 64 MiB record budget")
        metrics["selected_record_bytes"] = selected_record_bytes

        subscriber_dir = Path(subscriber.data) / "lists" / pubkey
        network_dir = Path(subscriber.data) / "lists" / ".network"
        cache_dir = Path(subscriber.data) / "mesh-local" / pubkey / "cache"
        metrics["subscriber_record_disk_bytes"] = tree_bytes(subscriber_dir)
        metrics["subscriber_record_file_count"] = tree_file_count(subscriber_dir)
        metrics["torrent_working_copy_bytes_before_media"] = tree_bytes(network_dir)
        metrics["media_cache_bytes_before_fetch"] = tree_bytes(cache_dir)

        before_media = loopback_counters()
        started = time.monotonic()
        poster_path = f"/api/catalog/{1:040x}/poster?list={pubkey}"
        poster_result = wait_for(
            lambda: (lambda response: response if response[0] == 200 else None)(
                get_raw(subscriber, poster_path)
            ),
            seconds=90, interval=1.0, label="lazy maximum-size poster fetch"
        )
        metrics["poster_fetch_seconds"] = round(time.monotonic() - started, 3)
        check(poster_result[1] == poster, "lazy poster fetch returns verified 2 MiB bytes")
        check((cache_dir / poster_ref["sha256"]).stat().st_size == POSTER_LIMIT,
              "poster object cached by content hash")
        started = time.monotonic()
        description_path = f"/api/catalog/{1:040x}/description?list={pubkey}"
        description_result = wait_for(
            lambda: (lambda response: response if response[0] == 200 else None)(
                get_raw(subscriber, description_path)
            ),
            seconds=60, interval=1.0, label="lazy maximum-size description fetch"
        )
        metrics["description_fetch_seconds"] = round(time.monotonic() - started, 3)
        check(description_result[1] == description, "lazy description fetch returns verified 64 KiB bytes")
        check((cache_dir / description_ref["sha256"]).stat().st_size == DESCRIPTION_LIMIT,
              "description object cached by content hash")
        after_media = loopback_counters()
        metrics["lazy_media_loopback_rx_bytes"] = after_media["rx"] - before_media["rx"]
        metrics["lazy_media_loopback_tx_bytes"] = after_media["tx"] - before_media["tx"]
        metrics["subscriber_media_cache_bytes"] = tree_bytes(cache_dir)
        metrics["torrent_working_copy_bytes_after_media"] = tree_bytes(network_dir)
        check(metrics["subscriber_media_cache_bytes"] == DESCRIPTION_LIMIT + POSTER_LIMIT,
              "lazy cache is bounded to the two requested media objects")
        check(not any((subscriber_dir / "media").glob("*.bin")) if (subscriber_dir / "media").exists() else True,
              "lazy media requests do not silently enable full-pack mirroring")

        # Media is now verified and cached. Stop the publisher to prove that the
        # subscriber can cold-start and serve both objects entirely from local cache.
        memory_before_restart = proc_memory(subscriber.proc.pid)
        publisher.stop()
        subscriber.stop()
        started = time.monotonic()
        subscriber.start()
        metrics["cold_start_seconds"] = round(time.monotonic() - started, 3)
        memory_after_restart = proc_memory(subscriber.proc.pid)
        status, result = subscriber.call(f"/api/catalog/search?q={query}&limit=5")
        check(status == 200 and any(r["hash"] == f"{1235:040x}" for r in result["results"]),
              "large catalog index survives a cold daemon restart")
        restarted_list = next(x for x in subscriber.lists() if x["pubkey"] == pubkey)
        check(restarted_list["records"] == MOVIE_RECORDS,
              "all selected records remain indexed after restart")
        check(get_raw(subscriber, poster_path)[1] == poster
              and get_raw(subscriber, description_path)[1] == description,
              "description and poster remain available offline after restart")
        metrics["rss_before_restart_bytes"] = memory_before_restart.get("VmRSS", 0)
        metrics["rss_high_water_before_restart_bytes"] = memory_before_restart.get("VmHWM", 0)
        metrics["rss_after_restart_bytes"] = memory_after_restart.get("VmRSS", 0)
        metrics["rss_high_water_after_restart_bytes"] = memory_after_restart.get("VmHWM", 0)
        metrics["profile"] = "20k selected records + 512 unselected records"
        metrics["record_segments"] = len(movie_segments) + len(series_segments)
        metrics["record_budget_bytes"] = RECORD_BUDGET
        metrics["media_budget_bytes"] = MEDIA_BUDGET
        metrics["cache_budget_bytes"] = CACHE_BUDGET
        metrics["publisher_disk_bytes_final"] = tree_bytes(publisher_dir)
        metrics["subscriber_disk_bytes_final"] = tree_bytes(subscriber_dir)
        print("MESH_SCALE_METRICS " + json.dumps(metrics, sort_keys=True), flush=True)
        summary = metrics_summary(metrics)
        summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
        if summary_path:
            with open(summary_path, "a", encoding="utf-8") as stream:
                stream.write(summary)
        print("PASS MediaMesh large-catalog resource profile", flush=True)
    except Exception:
        for node in nodes:
            try:
                log = Path(node.data) / "stdout.log"
                if log.exists():
                    print(f"---- {Path(node.data).name} daemon log ----", flush=True)
                    print(log.read_text(errors="replace")[-10000:], flush=True)
            except Exception:
                pass
        raise
    finally:
        for node in nodes:
            try:
                node.stop()
            except Exception:
                pass
        if not os.environ.get("KEEP"):
            shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
