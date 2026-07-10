#!/usr/bin/env python3
"""Summarize paired prefetch A/B benchmark runs."""

from __future__ import annotations

import argparse
import json
import statistics
from collections import defaultdict
from pathlib import Path


TOP_LEVEL_METRICS = (
    "avg_query_latency_ms",
    "latency_p95_ms",
    "latency_p99_ms",
    "qps",
    "recall_at_k",
    "cache_hit_rate",
)
STAT_METRICS = (
    "page_reads",
    "demand_reads",
    "prefetch_submitted",
    "prefetch_ready_hit",
    "prefetch_pending_hit",
    "prefetch_useful_pages",
    "prefetch_wasted_pages",
)


def load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def metric_value(payload: dict, name: str) -> float | None:
    if name in payload:
        value = payload.get(name)
    else:
        value = payload.get("stats", {}).get(name)
    return float(value) if isinstance(value, (int, float)) else None


def describe(values: list[float]) -> dict:
    if not values:
        return {
            "count": 0,
            "mean": None,
            "median": None,
            "stdev": None,
            "min": None,
            "max": None,
        }
    return {
        "count": len(values),
        "mean": statistics.fmean(values),
        "median": statistics.median(values),
        "stdev": statistics.stdev(values) if len(values) > 1 else 0.0,
        "min": min(values),
        "max": max(values),
    }


def summarize_cache(cache_dir: Path) -> dict:
    rounds = []
    deltas = defaultdict(list)
    trigger_totals = defaultdict(lambda: defaultdict(float))
    validators_ok = True
    replay_gaps = []
    replays_evaluable = True

    for round_dir in sorted(cache_dir.glob("round-*")):
        baseline_path = round_dir / "baseline.json"
        candidate_path = round_dir / "candidate.json"
        if not baseline_path.exists() or not candidate_path.exists():
            continue
        baseline = load_json(baseline_path)
        candidate = load_json(candidate_path)
        round_payload = {
            "round": round_dir.name,
            "seed": candidate.get("seed"),
            "order": (round_dir / "order.txt").read_text(encoding="utf-8").strip()
            if (round_dir / "order.txt").exists()
            else None,
            "baseline": str(baseline_path),
            "candidate": str(candidate_path),
            "metrics": {},
        }
        for name in TOP_LEVEL_METRICS + STAT_METRICS:
            baseline_value = metric_value(baseline, name)
            candidate_value = metric_value(candidate, name)
            delta = (
                candidate_value - baseline_value
                if baseline_value is not None and candidate_value is not None
                else None
            )
            round_payload["metrics"][name] = {
                "baseline": baseline_value,
                "candidate": candidate_value,
                "delta": delta,
            }
            if delta is not None:
                deltas[name].append(delta)

        trace_summary_path = round_dir / "candidate.trace.summary.json"
        if trace_summary_path.exists():
            trace_summary = load_json(trace_summary_path)
            round_payload["trace_summary"] = trace_summary.get("global")
            for trigger, metrics in trace_summary.get("by_trigger", {}).items():
                for key in (
                    "submitted",
                    "ready_hit",
                    "pending_hit",
                    "useful",
                    "wasted",
                    "saved_wait_us_sum",
                ):
                    value = metrics.get(key)
                    if isinstance(value, (int, float)):
                        trigger_totals[trigger][key] += value

        validation_path = round_dir / "candidate.trace.validation.json"
        if validation_path.exists():
            validation = load_json(validation_path)
            round_payload["validation_status"] = validation.get("status")
            validators_ok = validators_ok and validation.get("status") == "ok"
        else:
            validators_ok = False
            round_payload["validation_status"] = "missing"

        replay_path = round_dir / "candidate.replay.json"
        if replay_path.exists():
            replay = load_json(replay_path)
            xgboost_rows = [
                row for row in replay.get("results", []) if row.get("policy") == "xgboost"
            ]
            round_replay_evaluable = bool(xgboost_rows) and all(
                row.get("evaluable") is True for row in xgboost_rows
            )
            round_payload["replay_evaluable"] = round_replay_evaluable
            replays_evaluable = replays_evaluable and round_replay_evaluable
            gaps = [
                row.get("absolute_useful_rate_gap_pp")
                for row in xgboost_rows
                if row.get("evaluable") is True
                and isinstance(row.get("absolute_useful_rate_gap_pp"), (int, float))
            ]
            if round_replay_evaluable and gaps:
                round_gap = min(gaps)
                replay_gaps.append(round_gap)
                round_payload["replay_useful_rate_gap_pp"] = round_gap
        else:
            replays_evaluable = False
            round_payload["replay_evaluable"] = False

        rounds.append(round_payload)

    all_replays_evaluable = replays_evaluable and bool(rounds)
    return {
        "cache_condition": cache_dir.name,
        "round_count": len(rounds),
        "rounds": rounds,
        "paired_delta_summary": {
            name: describe(values) for name, values in sorted(deltas.items())
        },
        "trigger_totals": {
            trigger: dict(sorted(values.items()))
            for trigger, values in sorted(trigger_totals.items())
        },
        "all_trace_validations_ok": validators_ok and bool(rounds),
        "all_replays_evaluable": all_replays_evaluable,
        "max_replay_useful_rate_gap_pp": max(replay_gaps) if replay_gaps else None,
        "replay_gap_under_5pp": all_replays_evaluable
        and len(replay_gaps) == len(rounds)
        and max(replay_gaps) < 5.0,
    }


def render_markdown(payload: dict) -> str:
    lines = ["# Prefetch A/B Report", "", f"Suite: `{payload['suite_id']}`", ""]
    for cache_name, summary in sorted(payload["cache_conditions"].items()):
        lines.extend(
            [
                f"## {cache_name}",
                "",
                f"- Paired rounds: {summary['round_count']}",
                f"- Trace validation: {summary['all_trace_validations_ok']}",
                f"- Replay evaluable: {summary['all_replays_evaluable']}",
                f"- Replay gap < 5pp: {summary['replay_gap_under_5pp']}",
                "",
                "| Metric | Median Delta | Mean Delta | Min | Max |",
                "| --- | ---: | ---: | ---: | ---: |",
            ]
        )
        for name, stats in summary["paired_delta_summary"].items():
            lines.append(
                f"| `{name}` | {stats['median']} | {stats['mean']} | "
                f"{stats['min']} | {stats['max']} |"
            )
        if summary["trigger_totals"]:
            lines.extend(
                [
                    "",
                    "### Trigger Totals",
                    "",
                    "| Trigger | Submitted | Ready | Pending | Useful | Wasted | Saved Wait (us) |",
                    "| --- | ---: | ---: | ---: | ---: | ---: | ---: |",
                ]
            )
            for trigger, metrics in summary["trigger_totals"].items():
                lines.append(
                    f"| `{trigger}` | {metrics.get('submitted', 0)} | "
                    f"{metrics.get('ready_hit', 0)} | "
                    f"{metrics.get('pending_hit', 0)} | "
                    f"{metrics.get('useful', 0)} | "
                    f"{metrics.get('wasted', 0)} | "
                    f"{metrics.get('saved_wait_us_sum', 0)} |"
                )
        lines.append("")
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description="Summarize prefetch A/B runs.")
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--md-out", type=Path)
    args = parser.parse_args()

    payload = {
        "suite_id": args.root.name,
        "root": str(args.root),
        "cache_conditions": {
            cache_dir.name: summarize_cache(cache_dir)
            for cache_dir in sorted(args.root.iterdir())
            if cache_dir.is_dir()
        },
    }
    payload["acceptance_ok"] = bool(payload["cache_conditions"]) and all(
        summary["round_count"] >= 5
        and summary["all_trace_validations_ok"]
        and summary["all_replays_evaluable"]
        and summary["replay_gap_under_5pp"]
        for summary in payload["cache_conditions"].values()
    )

    json_out = args.json_out or args.root / "report.json"
    md_out = args.md_out or args.root / "report.md"
    json_out.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    md_out.write_text(render_markdown(payload) + "\n", encoding="utf-8")
    print(json.dumps(payload, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
