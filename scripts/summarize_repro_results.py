#!/usr/bin/env python3
"""Summarize results/<run>/*.json into Markdown tables for the docs.

Reads the raw JSON produced by ``run_repro_experiments.py`` and prints
median/min/max over the rounds of each configuration. Nothing is invented:
missing fields (e.g. recall when no samples were taken) render as "-".

Tables and column sets must stay in sync with scripts/summarize_repro_results.cpp
(the no-Python variant used on the Windows benchmark host).
"""

from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path


def median(values):
    return statistics.median(values) if values else None


def fmt(value, digits=4):
    if value is None:
        return "-"
    if isinstance(value, bool):
        return "yes" if value else "no"
    if isinstance(value, int) or digits == 0:
        return str(int(value))
    return f"{value:.{digits}f}"


def range_text(values, digits=4):
    if not values:
        return "-"
    return (
        f"{fmt(median(values), digits)} "
        f"({fmt(min(values), digits)}-{fmt(max(values), digits)})"
    )


def load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def rounds_in(directory: Path) -> list[dict]:
    paths = sorted(directory.glob("round*.json"))
    return [load_json(path) for path in paths]


def pick(rounds: list[dict], *keys):
    """First key present in every round wins (newer JSON uses nested names)."""
    for key in keys:
        values = [r.get(key) for r in rounds if r.get(key) is not None]
        if values:
            return values
    return []


MIB = 1024 * 1024


def search_table(run: Path) -> str:
    lines = [
        "| cache policy | search width | Recall@10 | Avg ms | P95 ms | P99 ms | "
        "P999 ms | QPS | reads/query | cache hit | peak RSS (MB) |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    policy_dirs = sorted(p for p in run.iterdir()
                         if p.is_dir() and p.name.startswith("search-"))
    for policy_dir in policy_dirs:
        width_dirs = sorted(
            (p for p in policy_dir.iterdir() if p.is_dir()),
            key=lambda p: int(p.name.removeprefix("sw")))
        for width_dir in width_dirs:
            rounds = rounds_in(width_dir)
            if not rounds:
                continue
            rss = [v / MIB for v in pick(rounds, "measured_peak_rss_bytes")]
            lines.append(
                "| " + str(rounds[0].get("cache_policy")) + " | "
                + str(rounds[0].get("effective_search_width")) + " | "
                + range_text(pick(rounds, "recall_at_k"), 4) + " | "
                + range_text(pick(rounds, "avg_query_latency_ms"), 4) + " | "
                + range_text(pick(rounds, "latency_p95_ms"), 4) + " | "
                + range_text(pick(rounds, "latency_p99_ms"), 4) + " | "
                + range_text(pick(rounds, "latency_p999_ms"), 4) + " | "
                + range_text(pick(rounds, "qps"), 1) + " | "
                + range_text(pick(rounds, "reads_per_query"), 2) + " | "
                + range_text(pick(rounds, "cache_hit_rate"), 4) + " | "
                + range_text(rss, 2) + " |")
    return "\n".join(lines)


def mixed_scenario(path: Path) -> dict:
    # Two layouts: without --duration_sec the single unnamed scenario leaves the
    # measurements at the top level; with --duration_sec (how the experiment
    # driver runs it) they are nested under "scenarios"[0]. The driver emits
    # exactly one scenario per file, so the first one is the intended one.
    data = load_json(path)
    if "read_qps" in data:
        return data
    if "scenarios" in data and data["scenarios"]:
        return data["scenarios"][0]
    raise SystemExit(f"{path}: no measured scenario found")


def mixed_table(run: Path) -> str:
    lines = [
        "| config | Read QPS | Write QPS | Read P95 ms | Read P99 ms | "
        "Recovery ms | Delta search ms/read | Compaction ok/attempt | "
        "SSTables | Peak RSS MB |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    mixed_dir = run / "mixed"
    for config_dir in sorted(p for p in mixed_dir.iterdir() if p.is_dir()):
        rounds = [mixed_scenario(p)
                  for p in sorted(config_dir.glob("round*.json"))]
        if not rounds:
            continue
        delta = pick(rounds, "read_breakdown.delta_search_avg_ms",
                     "delta_search_avg_ms")
        compaction = [f"{r.get('compaction_count', 0)}/"
                      f"{r.get('compaction_attempted', 0)}" for r in rounds]
        rss = [v / MIB for v in pick(rounds, "process_peak_rss_bytes")]
        lines.append(
            f"| {config_dir.name} | " + range_text(pick(rounds, "read_qps"), 1) + " | "
            + range_text(pick(rounds, "write_qps"), 2) + " | "
            + range_text(pick(rounds, "read_p95_ms"), 3) + " | "
            + range_text(pick(rounds, "read_p99_ms"), 3) + " | "
            + range_text(pick(rounds, "recovery_time_ms"), 2) + " | "
            + range_text(delta, 3) + " | "
            + ", ".join(compaction) + " | "
            + range_text(pick(rounds, "sstable_count"), 0) + " | "
            + range_text(rss, 2) + " |")
    return "\n".join(lines)


def recovery_table(run: Path) -> str:
    lines = [
        "| file | status | writes committed | visible delta records | SSTables "
        "| WAL segments | WAL records replayed | truncated WAL segments | "
        "recovery ms |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    failure_dir = run / "failure"
    for path in sorted(p for p in failure_dir.iterdir() if p.is_file()):
        flat = mixed_scenario(path)
        get = lambda key: flat.get(key, "-")
        lines.append(
            f"| {path.name} | {get('status')} | {get('committed_write_records')} | "
            f"{get('recovery.visible_delta_records')} | "
            f"{get('recovery.sstable_count')} | {get('recovery.wal_segments')} | "
            f"{get('recovery.wal_records')} | "
            f"{get('recovery.wal_truncated_segments')} | "
            f"{get('recovery_time_ms')} |")
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("run", type=Path, help="results/<run directory>")
    parser.add_argument("--section", choices=("search", "mixed", "recovery", "all"),
                        default="all")
    args = parser.parse_args()
    if args.section in ("search", "all"):
        print("### Search matrix\n")
        print(search_table(args.run))
        print()
    if args.section in ("mixed", "all"):
        print("### Mixed read/write\n")
        print(mixed_table(args.run))
        print()
    if args.section in ("recovery", "all"):
        print("### Recovery check\n")
        print(recovery_table(args.run))
        print()


if __name__ == "__main__":
    main()
