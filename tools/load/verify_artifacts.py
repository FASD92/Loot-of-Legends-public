#!/usr/bin/env python3
"""Verify a closed load-test evidence package without changing it."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.load.loot_load.evidence.package import verify_package  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", type=Path, required=True)
    args = parser.parse_args()
    package_dir = args.run_dir / "package"
    result = verify_package(package_dir if package_dir.is_dir() else args.run_dir)
    print(result["packageStatus"])
    return 0 if result["packageStatus"] == "COMPLETE" else 1


if __name__ == "__main__":
    raise SystemExit(main())
