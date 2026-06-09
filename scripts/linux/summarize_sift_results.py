#!/usr/bin/env python3
"""Summarize key=value SIFT experiment logs and recommend graph profiles."""

from __future__ import annotations

import csv
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RESULT_DIR = ROOT / "archive" / "results"
OUTPUT = RESULT_DIR / "sift_summary.csv"

FIELDS = [
    "file_name",
    "timestamp",
    "dataset",
    "run_cache_mode",
    "warmup_runs",
    "engine",
    "base_count",
    "query_count",
    "packing_strategy",
    "hotpath_train_queries",
    "hotpath_unique_visited_nodes",
    "hotpath_top_node_visit_count",
    "search_width",
    "search_early_stop_min_expansions",
    "adaptive_early_stop",
    "early_stop_patience",
    "early_stop_eps",
    "min_expansions",
    "max_expansions",
    "cache_policy",
    "path_cache_policy",
    "pq_enable",
    "pq_m",
    "pq_ks",
    "pq_code_bytes_per_vector",
    "pq_train_seconds",
    "adc_enable",
    "adc_table_build_us_avg",
    "rerank_topk",
    "io_mode",
    "io_mode_effective",
    "io_direct_enabled",
    "io_uring_enabled",
    "io_fallback_reason",
    "recall_at_10",
    "qps",
    "avg_latency_ms",
    "p50_latency_ms",
    "p95_latency_ms",
    "p99_latency_ms",
    "ssd_reads_per_query_avg",
    "ssd_reads_per_query_p95",
    "ssd_reads_per_query_p99",
    "graph_expanded_per_query_avg",
    "graph_expanded_per_query_p95",
    "graph_expanded_per_query_p99",
    "graph_visited_per_query_avg",
    "graph_visited_per_query_p95",
    "graph_visited_per_query_p99",
    "io_submit_count_per_query_avg",
    "io_complete_count_per_query_avg",
    "io_batch_size",
    "io_amplification_reads_per_result",
    "page_cache_hit_rate",
    "memory_budget_ratio",
    "memory_budget_bytes",
    "memory_budget_bytes_user",
    "memory_budget_enforced",
    "memory_over_budget_allowed",
    "memory_resident_bytes",
    "memory_resident_ratio",
    "memory_budget_pass",
    "memory_mode",
    "memory_accounting_scope",
    "memory_bytes_raw_vectors",
    "memory_bytes_raw_vectors_resident",
    "memory_bytes_pq_codes",
    "memory_bytes_pq_codebooks",
    "memory_bytes_graph_metadata",
    "memory_bytes_cache",
    "memory_bytes_path_cache",
    "memory_bytes_router",
    "memory_bytes_delta",
    "memory_bytes_tombstone",
    "memory_bytes_temporary_peak",
    "memory_compression_ratio",
    "elapsed_seconds",
    "index_build_seconds",
    "index_size_bytes",
]


def read_result_text(path: Path) -> str:
    data = path.read_bytes()
    if data.startswith((b"\xff\xfe", b"\xfe\xff")) or b"\x00" in data[:256]:
        return data.decode("utf-16", errors="replace")
    return data.decode("utf-8-sig", errors="replace")


def parse_file(path: Path) -> dict[str, str]:
    row: dict[str, str] = {"file_name": path.name, "timestamp": path.stem}
    for line in read_result_text(path).splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key and " " not in key:
            row[key.strip()] = value.strip()
    return row


def number(row: dict[str, str], key: str, default: float = float("inf")) -> float:
    try:
        return float(row.get(key, ""))
    except ValueError:
        return default


def markdown(rows: list[dict[str, str]]) -> None:
    columns = [
        "file_name",
        "engine",
        "base_count",
        "run_cache_mode",
        "packing_strategy",
        "search_width",
        "search_early_stop_min_expansions",
        "recall_at_10",
        "qps",
        "p99_latency_ms",
        "ssd_reads_per_query_avg",
        "pq_enable",
        "adc_enable",
        "io_mode_effective",
        "memory_budget_pass",
        "memory_resident_ratio",
    ]
    print("| " + " | ".join(columns) + " |")
    print("|" + "|".join(["---"] * len(columns)) + "|")
    for row in rows:
        print("| " + " | ".join(row.get(column, "") for column in columns) + " |")


def recommendations(rows: list[dict[str, str]]) -> None:
    bases = sorted({row.get("base_count", "") for row in rows if row.get("engine") == "graph"})
    for base in bases:
        eligible = [
            row
            for row in rows
            if row.get("engine") == "graph"
            and row.get("base_count") == base
            and number(row, "recall_at_10", 0.0) >= 0.95
            and row.get("memory_budget_pass", "1") != "0"
        ]
        if not eligible:
            print(f"\nNo graph config for SIFT{base} reached Recall@10 >= 0.95")
            continue
        best = min(
            eligible,
            key=lambda row: (
                number(row, "p99_latency_ms"),
                number(row, "ssd_reads_per_query_avg"),
                -number(row, "qps", 0.0),
            ),
        )
        print(f"\nBest graph config for SIFT{base}:")
        print(f"SEARCH_WIDTH={best.get('search_width', '')}")
        print(
            "SEARCH_EARLY_STOP_MIN="
            f"{best.get('search_early_stop_min_expansions', '')}"
        )
        print(f"PACKING_STRATEGY={best.get('packing_strategy', '')}")
        print(f"Recall@10={best.get('recall_at_10', '')}")
        print(f"P99={best.get('p99_latency_ms', '')}")
        print(f"SSD reads/query={best.get('ssd_reads_per_query_avg', '')}")
        print(f"QPS={best.get('qps', '')}")


def main() -> None:
    paths = sorted(RESULT_DIR.glob("sift-local-*.txt"))
    rows = [parse_file(path) for path in paths]
    with OUTPUT.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDS, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)
    print(f"Wrote {OUTPUT}")
    markdown(rows)
    recommendations(rows)


if __name__ == "__main__":
    main()
