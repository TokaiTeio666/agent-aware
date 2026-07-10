#!/usr/bin/env python3
"""Stream and summarize prefetch trace CSV files."""

from __future__ import annotations

import argparse
import json
from collections import defaultdict
from pathlib import Path

from prefetch_trace_utils import TraceAccumulator, iter_trace_rows


def metric_text(value: object) -> str:
    if value is None:
        return "null"
    if isinstance(value, float):
        return f"{value:.6g}"
    return str(value)


def render_markdown(payload: dict) -> str:
    lines = ["# Prefetch Trace Summary", ""]
    sections = [("Global", payload["global"])]
    sections.extend(
        (f"Trigger: {name}", metrics)
        for name, metrics in sorted(payload["by_trigger"].items())
    )
    if payload.get("groups"):
        sections.extend(
            (f"Group: {name}", metrics)
            for name, metrics in sorted(payload["groups"].items())
        )

    for title, metrics in sections:
        lines.extend(
            [
                f"## {title}",
                "",
                "| Metric | Value |",
                "| --- | ---: |",
            ]
        )
        for key in (
            "rows",
            "queries",
            "submitted",
            "ready_hit",
            "pending_hit",
            "useful",
            "wasted",
            "saved_wait_us_sum",
            "saved_wait_us_avg",
            "useful_rate",
            "ready_hit_rate",
            "pending_hit_rate",
            "waste_rate",
            "useful_identity_ok",
            "terminal_identity_ok",
        ):
            lines.append(f"| `{key}` | {metric_text(metrics.get(key))} |")
        lines.append("")
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description="Analyze prefetch trace CSV files.")
    parser.add_argument("--trace", nargs="+", required=True, type=Path)
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--md-out", type=Path)
    parser.add_argument(
        "--group-by", choices=("global", "query", "trigger"), default="global"
    )
    args = parser.parse_args()

    trace_names = [str(path) for path in args.trace]
    global_accumulator = TraceAccumulator()
    trigger_accumulators = defaultdict(TraceAccumulator)
    requested_groups = defaultdict(TraceAccumulator)

    for row in iter_trace_rows(args.trace):
        global_accumulator.add(row)
        trigger_accumulators[str(row.get("trigger") or "unknown")].add(row)
        if args.group_by == "query":
            requested_groups[str(row.get("query_id") or "unknown")].add(row)
        elif args.group_by == "trigger":
            requested_groups[str(row.get("trigger") or "unknown")].add(row)

    if global_accumulator.rows == 0:
        raise SystemExit("no trace rows found")

    payload = {
        "global": global_accumulator.finish(trace_names),
        "by_trigger": {
            name: accumulator.finish(trace_names)
            for name, accumulator in sorted(trigger_accumulators.items())
        },
    }
    if requested_groups:
        payload["groups"] = {
            name: accumulator.finish(trace_names)
            for name, accumulator in sorted(requested_groups.items())
        }

    text = json.dumps(payload, indent=2, sort_keys=True)
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(text + "\n", encoding="utf-8")
    if args.md_out:
        args.md_out.parent.mkdir(parents=True, exist_ok=True)
        args.md_out.write_text(render_markdown(payload) + "\n", encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
