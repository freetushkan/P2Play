#!/usr/bin/env python3
"""P2Play catalog grabber command-line entry point."""

from pathlib import Path
import sys

# Also works with Python isolated mode (-I), as used by the repository CI.
sys.path.insert(0, str(Path(__file__).resolve().parent))

from grabber_lib import main

if __name__ == "__main__":
    raise SystemExit(main())
