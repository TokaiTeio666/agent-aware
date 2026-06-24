#!/usr/bin/env python3
"""Summarize agent-aware prefetch trace CSV files."""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence


ALIASES = {
    "query_id": ("query_id", "qid"),
    "was_prefetched": ("was_prefetched", "prefetched", "prefetch_submitted"),
    "prefetch_submit_time": ("prefetch_submit_time", "submit_time", "submit_time_us"),
    "prefetch_ready_time": ("prefetch_ready_time", "ready_time", "ready_time_us"),
    "demand_time": ("demand_time", "demand_time_us", "access_time", "demand_access_time"),
    "was_demanded": ("was_demanded", "demanded", "was_used", "used"),
    "was_ready_before_demand": (
        "was_ready_before_demand",
        "ready_before_demand",
        "prefetch_ready_hit",
    ),
    "was_evicted_before_use": (
        "was_evicted_before_use",
        "evicted_before_use",
        "prefetch_dropped",
        "dropped",
    ),
    "label": ("label", "prefetch_label"),
}


def parse_bool(value: object) -> Optional[bool]:
    if value is None:
        return None
    text = str(value).strip().lower()
    if text in {"", "null", "none", "nan"}:
        return None
    if text in {"1", "true", "yes", "y", "on"}:
        return True
    if text in {"0", "false", "no", "n", "off", "-1"}:
        return False
    return None


def parse_float(value: object) -> Optional[float]:
    if value is None:
        return None
    text = str(value).strip()
    if text in {"", "-1", "null", "None", "nan"}:
        return None
    try:
        parsed = float(text)
    except ValueError:
        return None
    return parsed if math.isfinite(parsed) else None


def parse_int(value: object) -> Optional[int]:
    parsed = parse_float(value)
    if parsed is None:
        return None
    return int(parsed)


def first_present(row: dict, logical_name: str) -> object:
    for name in ALIASES[logical_name]:
        if name in row:
            return row.get(name)
    return None


def present_columns(rows: Sequence[dict]) -> set:
    columns = set()
    for row in rows:
        columns.update(row.keys())
    return columns


def missing_aliases(columns: set, logical_names: Iterable[str]) -> List[str]:
    missing = []
    for logical_name in logical_names:
        if not any(name in columns for name in ALIASES[logical_name]):
            missing.append(logical_name)
    return missing


def percentile(values: Sequence[float], pct: float) -> Optional[float]:
    if not values:
        return None
    ordered = sorted(values)
    rank = math.ceil((pct / 100.0) * len(ordered))
    rank = min(max(rank, 1), len(ordered))
    return ordered[rank - 1]


def safe_rate(numerator: Optional[int], denominator: Optional[int]) -> Optional[float]:
    if numerator is None or denominator is None or denominator == 0:
        return None
    return numerator / denominator


def summarize_rows(rows: Sequence[dict], trace_names: Sequence[str]) -> dict:
    columns = present_columns(rows)
    missing = missing_aliases(
        columns,
        [
            "query_id",
            "was_prefetched",
            "prefetch_ready_time",
            "demand_time",
            "was_demanded",
            "was_evicted_before_use",
        ],
    )

    query_ids = {
        parse_int(first_present(row, "query_id"))
        for row in rows
        if parse_int(first_present(row, "query_id")) is not None
    }
    queries = len(query_ids) if query_ids else None

    submitted = ready = demand_overlap = ready_before_demand = 0
    pending_at_demand = used = unused = dropped = 0
    lead_times: List[float] = []

    for row in rows:
        was_prefetched = parse_bool(first_present(row, "was_prefetched"))
        submit_time = parse_float(first_present(row, "prefetch_submit_time"))
        ready_time = parse_float(first_present(row, "prefetch_ready_time"))
        demand_time = parse_float(first_present(row, "demand_time"))
        was_demanded = parse_bool(first_present(row, "was_demanded"))
        ready_flag = parse_bool(first_present(row, "was_ready_before_demand"))
        dropped_flag = parse_bool(first_present(row, "was_evicted_before_use"))
        label = parse_int(first_present(row, "label"))

        prefetched = bool(was_prefetched) or submit_time is not None
        demanded = bool(was_demanded) or demand_time is not None or (
            label is not None and label > 0
        )

        if prefetched:
            submitted += 1
        if prefetched and ready_time is not None:
            ready += 1
        if prefetched and demanded:
            demand_overlap += 1
            used += 1
        if prefetched and not demanded:
            unused += 1
        if prefetched and dropped_flag:
            dropped += 1

        is_ready_before = False
        if prefetched and demanded:
            if ready_time is not None and demand_time is not None:
                is_ready_before = ready_time < demand_time
            elif ready_flag is not None:
                is_ready_before = ready_flag
        if is_ready_before:
            ready_before_demand += 1
            if ready_time is not None and demand_time is not None:
                lead_times.append(demand_time - ready_time)
        elif prefetched and demanded:
            pending_at_demand += 1

    metrics = {
        "trace_files": list(trace_names),
        "rows": len(rows),
        "queries": queries,
        "prefetch_submitted": submitted,
        "prefetch_ready": ready,
        "prefetch_demand_overlap": demand_overlap,
        "prefetch_ready_before_demand": ready_before_demand,
        "prefetch_pending_at_demand": pending_at_demand,
        "prefetch_used": used,
        "prefetch_unused": unused,
        "prefetch_dropped": dropped,
        "ready_hit_rate": safe_rate(ready_before_demand, submitted),
        "pending_hit_rate": safe_rate(pending_at_demand, demand_overlap),
        "useful_rate": safe_rate(used, submitted),
        "waste_rate": safe_rate(unused, submitted),
        "drop_rate": safe_rate(dropped, submitted),
        "avg_lead_time_us": statistics.fmean(lead_times) if lead_times else None,
        "p50_lead_time_us": percentile(lead_times, 50.0),
        "p95_lead_time_us": percentile(lead_times, 95.0),
        "p99_lead_time_us": percentile(lead_times, 99.0),
        "missing_fields": missing,
    }
    return metrics


def load_traces(paths: Sequence[Path]) -> List[dict]:
    rows = []
    for path in paths:
        with path.open(newline="") as handle:
            reader = csv.DictReader(handle)
            for row in reader:
                row["_trace_file"] = str(path)
                rows.append(row)
    return rows


def metric_value(metrics: dict, name: str) -> str:
    value = metrics.get(name)
    if value is None:
        return "null"
    if isinstance(value, float):
        return f"{value:.6g}"
    return str(value)


def make_diagnostics(metrics: dict) -> List[str]:
    diagnostics = []
    submitted = metrics.get("prefetch_submitted") or 0
    ready_rate = metrics.get("ready_hit_rate") or 0.0
    pending_rate = metrics.get("pending_hit_rate") or 0.0
    useful_rate = metrics.get("useful_rate") or 0.0
    waste_rate = metrics.get("waste_rate") or 0.0

    if pending_rate >= 0.5:
        diagnostics.append("pending_hit_rate 偏高：预取方向可能对，但触发太晚。")
    if waste_rate >= 0.3:
        diagnostics.append("waste_rate 偏高：预取可能污染 I/O，ranking 或 threshold 需要收紧。")
    if ready_rate >= 0.5:
        diagnostics.append(
            "ready_hit_rate 较高：若 benchmark latency 没下降，瓶颈可能不是 page I/O，而是距离计算、候选管理、锁或 cache。"
        )
    if submitted >= 1 and useful_rate < 0.3:
        diagnostics.append("submitted 较多但 useful_rate 较低：预取预算可能过大或 score threshold 太低。")
    if metrics.get("missing_fields"):
        diagnostics.append(
            "部分指标依赖字段缺失：" + ", ".join(metrics["missing_fields"]) + "。"
        )
    if not diagnostics:
        diagnostics.append("未发现明显异常；建议结合 A/B latency 和 replay oracle gap 判断。")
    return diagnostics


def markdown_for_summary(summary: dict) -> str:
    sections = ["# Prefetch Trace Summary", ""]
    groups = summary.get("groups") or {"global": summary["global"]}
    ordered_groups = []
    if "global" in summary:
        ordered_groups.append(("global", summary["global"]))
    ordered_groups.extend(
        (name, metrics) for name, metrics in groups.items() if name != "global"
    )

    for name, metrics in ordered_groups:
        title = "Global" if name == "global" else str(name)
        sections.extend([f"## {title}", ""])
        sections.extend(
            [
                "### Basic Counts",
                "",
                "| Metric | Value |",
                "| --- | ---: |",
            ]
        )
        for key in [
            "queries",
            "rows",
            "prefetch_submitted",
            "prefetch_ready",
            "prefetch_demand_overlap",
            "prefetch_ready_before_demand",
            "prefetch_pending_at_demand",
            "prefetch_used",
            "prefetch_unused",
            "prefetch_dropped",
        ]:
            sections.append(f"| `{key}` | {metric_value(metrics, key)} |")
        sections.extend(["", "### Rates", "", "| Metric | Value |", "| --- | ---: |"])
        for key in [
            "ready_hit_rate",
            "pending_hit_rate",
            "useful_rate",
            "waste_rate",
            "drop_rate",
        ]:
            sections.append(f"| `{key}` | {metric_value(metrics, key)} |")
        sections.extend(["", "### Lead Time", "", "| Metric | Value |", "| --- | ---: |"])
        for key in [
            "avg_lead_time_us",
            "p50_lead_time_us",
            "p95_lead_time_us",
            "p99_lead_time_us",
        ]:
            sections.append(f"| `{key}` | {metric_value(metrics, key)} |")
        sections.extend(["", "### Diagnostics", ""])
        for diagnostic in make_diagnostics(metrics):
            sections.append(f"- {diagnostic}")
        sections.append("")
    return "\n".join(sections)


def main() -> None:
    parser = argparse.ArgumentParser(description="Analyze prefetch trace CSV files.")
    parser.add_argument("--trace", nargs="+", required=True, type=Path)
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--md-out", type=Path)
    parser.add_argument(
        "--group-by", choices=("global", "query", "trigger"), default="global"
    )
    args = parser.parse_args()

    rows = load_traces(args.trace)
    if not rows:
        raise SystemExit("no trace rows found")

    global_summary = summarize_rows(rows, [str(path) for path in args.trace])
    summary = {"global": global_summary}

    if args.group_by in {"query", "trigger"}:
        grouped: Dict[str, List[dict]] = defaultdict(list)
        for row in rows:
            if args.group_by == "query":
                group_name = f"query {first_present(row, 'query_id')}"
            else:
                group_name = f"trigger {row.get('trigger') or 'unknown'}"
            grouped[str(group_name)].append(row)
        summary["groups"] = {
            group_name: summarize_rows(group_rows, [str(path) for path in args.trace])
            for group_name, group_rows in sorted(grouped.items())
        }
    else:
        summary["groups"] = {"global": global_summary}

    payload = json.dumps(summary, indent=2, sort_keys=True)
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(payload + "\n", encoding="utf-8")
    if args.md_out:
        args.md_out.parent.mkdir(parents=True, exist_ok=True)
        args.md_out.write_text(markdown_for_summary(summary), encoding="utf-8")
    print(payload)


if __name__ == "__main__":
    main()
