#!/usr/bin/env python3
"""Build a comparison CSV from real agent-aware and external system logs."""

from __future__ import annotations

import csv
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RESULT_DIR = ROOT / "archive" / "results"
OUTPUT = RESULT_DIR / "system_comparison.csv"

FIELDS = [
    "file_name",
    "system_name",
    "dataset",
    "base_count",
    "query_count",
    "k",
    "run_cache_mode",
    "recall_at_10",
    "qps",
    "avg_latency_ms",
    "p95_latency_ms",
    "p99_latency_ms",
    "index_build_seconds",
    "index_size_bytes",
    "memory_usage_mb",
    "memory_budget_ratio",
    "memory_budget_bytes",
    "memory_resident_bytes",
    "memory_resident_ratio",
    "memory_budget_pass",
    "memory_accounting_scope",
    "ssd_reads_per_query_avg",
    "status",
    "notes",
]


def read_result_text(path: Path) -> str:
    data = path.read_bytes()
    if data.startswith((b"\xff\xfe", b"\xfe\xff")) or b"\x00" in data[:256]:
        return data.decode("utf-16", errors="replace")
    return data.decode("utf-8-sig", errors="replace")


def parse_file(path: Path) -> dict[str, str]:
    row: dict[str, str] = {"file_name": path.name}
    for line in read_result_text(path).splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key and " " not in key:
            row[key.strip()] = value.strip()
    if path.name.startswith("sift-local-"):
        row.setdefault("system_name", "agent-aware")
        row.setdefault("status", "completed")
    if row.get("io_fallback_reason") and not row.get("notes"):
        row["notes"] = row["io_fallback_reason"]
    return row


def main() -> None:
    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    paths = sorted(RESULT_DIR.glob("sift-local-*.txt"))
    paths += sorted(RESULT_DIR.glob("system-*.txt"))
    rows = [parse_file(path) for path in paths]
    with OUTPUT.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDS, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)
    print(f"Wrote {OUTPUT}")


if __name__ == "__main__":
    main()
