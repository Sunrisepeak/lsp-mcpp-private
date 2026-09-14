#!/usr/bin/env python3
"""Medians of the startup timeline in a directory of conformance --measure files.

    timing_summary.py <directory>

Files are named cold-<round>.json and warm-<round>.json (nightly.yml). Prints a
Markdown table: seconds from initialize to the first ready state, the first
diagnostics and the first navigation, median over the rounds, with every value.
"""
import json
import pathlib
import statistics
import sys

FIELDS = ("ready", "first-diagnostics", "first-navigation")


def main() -> int:
    directory = pathlib.Path(sys.argv[1])
    runs: dict[str, list[dict]] = {"cold": [], "warm": []}
    for path in sorted(directory.glob("*.json")):
        kind = path.stem.split("-")[0]
        if kind in runs:
            runs[kind].append(json.loads(path.read_text(encoding="utf-8")))
    print("| start | " + " | ".join(FIELDS) + " | failures |")
    print("|---|" + "---|" * (len(FIELDS) + 1))
    for kind, measured in runs.items():
        if not measured:
            continue
        cells = []
        for field in FIELDS:
            values = [m[field] for m in measured if isinstance(m.get(field), (int, float))]
            if not values:
                cells.append("n/a")
                continue
            cells.append(f"{statistics.median(values):.2f} s ({', '.join(f'{v:.2f}' for v in values)})")
        failures = sum(m.get("failures", 0) for m in measured)
        print(f"| {kind} | " + " | ".join(cells) + f" | {failures} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
