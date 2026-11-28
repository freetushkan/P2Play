#!/usr/bin/env python3
"""Run MediaMesh tests in an isolated copy of the exact upstream Silo tree.

No changes to Silo or the native P2Play core are made. Requires git and Go >=1.26.
An existing checkout can avoid a network clone: --upstream /path/to/silo-source.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

PIN = "3610db846fd2132f06aa626a4cb94f4a0cfa69ae"
# This upstream repository carries the Silo plugin host branch used only by this test harness.
UPSTREAM = "https://github.com/YouROK/TorrServer.git"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upstream", type=Path, help="clean checkout at the pinned commit")
    parser.add_argument("--go", default="go", help="Go executable")
    parser.add_argument("--race", action="store_true", help="enable Go race detection (requires a C compiler)")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="silo-plugin-test-") as tmp:
        tree = Path(tmp) / "upstream"
        if args.upstream:
            source = args.upstream.resolve()
            head = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
            if head != PIN:
                parser.error(f"upstream HEAD must be {PIN}, got {head}")
            if subprocess.check_output(["git", "-C", str(source), "status", "--porcelain"], text=True).strip():
                parser.error("--upstream must be clean; tests never modify it")
            shutil.copytree(source, tree, ignore=shutil.ignore_patterns(".git"))
        else:
            subprocess.run(["git", "clone", "--branch", "silo", UPSTREAM, str(tree)], check=True)
            subprocess.run(["git", "-C", str(tree), "checkout", "--detach", PIN], check=True)
        shutil.copyfile(root / "tools/silo-plugin-test.go", tree / "internal/plugin/mediamesh_test.go")
        env = dict(os.environ, MEDIAMESH_PLUGIN_DIR=str(root / "plugins/mediamesh"))
        # Pinned upstream has no external build scripts needed for these Go tests.
        command = [args.go, "test"]
        if args.race:
            command.append("-race")
        command += ["./internal/plugin", "-run", "^TestMediaMesh", "-count=1", "-v"]
        subprocess.run(command, cwd=tree, env=env, check=True)


if __name__ == "__main__":
    main()
