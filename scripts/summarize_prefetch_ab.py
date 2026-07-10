#!/usr/bin/env python3
"""Summarize paired three-arm Stage 2 prefetch benchmark runs."""

from __future__ import annotations

import argparse
import json
import statistics
from collections import defaultdict
from pathlib import Path


ARMS = ("no_prefetch", "stage1_prebeam", "stage2_composite")
BASELINES = ("no_prefetch", "stage1_prebeam")
METRICS = (
    "avg_query_latency_ms",
    "latency_p95_ms",
    "latency_p99_ms",
    "qps",
    "recall_at_k",
    "submitted_reads",
    "prefetch_submitted",
    "prefetch_ready_hit",
    "prefetch_pending_hit",
    "prefetch_useful_pages",
    "prefetch_wasted_pages",
)


def load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def metric_value(payload: dict, name: str) -> float | None:
    value = payload.get(name)
    if value is None:
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


def ratio(numerator: float, denominator: float) -> float | None:
    return numerator / denominator if denominator else None


def median(summary: dict, comparison: str, metric: str) -> float | None:
    return summary.get(comparison, {}).get(metric, {}).get("median")


def summarize_cache(cache_dir: Path) -> dict:
    rounds = []
    absolute_deltas = defaultdict(lambda: defaultdict(list))
    relative_deltas = defaultdict(lambda: defaultdict(list))
    trigger_totals = defaultdict(lambda: defaultdict(float))
    validations_ok = True
    replays_evaluable = True
    stage2_submit_top_ks = []

    for round_dir in sorted(cache_dir.glob("round-*")):
        arm_paths = {arm: round_dir / f"{arm}.json" for arm in ARMS}
        if not all(path.exists() for path in arm_paths.values()):
            continue
        arms = {arm: load_json(path) for arm, path in arm_paths.items()}
        stage2_submit_top_ks.append(
            int(arms["stage2_composite"].get("prefetch_submit_top_k", 0))
        )
        round_payload = {
            "round": round_dir.name,
            "seed": arms["stage2_composite"].get("seed"),
            "order": (round_dir / "order.txt").read_text(encoding="utf-8").strip()
            if (round_dir / "order.txt").exists()
            else None,
            "arms": {arm: str(path) for arm, path in arm_paths.items()},
            "metrics": {},
        }

        for name in METRICS:
            values = {arm: metric_value(payload, name) for arm, payload in arms.items()}
            round_payload["metrics"][name] = values
            stage2 = values["stage2_composite"]
            for baseline in BASELINES:
                baseline_value = values[baseline]
                if stage2 is None or baseline_value is None:
                    continue
                comparison = f"stage2_vs_{baseline}"
                absolute_deltas[comparison][name].append(stage2 - baseline_value)
                if baseline_value != 0:
                    relative_deltas[comparison][name].append(
                        (stage2 - baseline_value) / baseline_value
                    )

        for arm in ("stage1_prebeam", "stage2_composite"):
            validation_path = round_dir / f"{arm}.trace.validation.json"
            validation_status = "missing"
            if validation_path.exists():
                validation_status = load_json(validation_path).get("status", "missing")
            round_payload[f"{arm}_validation_status"] = validation_status
            validations_ok = validations_ok and validation_status == "ok"

            replay_path = round_dir / f"{arm}.replay.json"
            arm_replay_evaluable = False
            if replay_path.exists():
                replay = load_json(replay_path)
                rows = [row for row in replay.get("results", []) if row.get("policy") == "xgboost"]
                arm_replay_evaluable = bool(rows) and all(
                    row.get("evaluable") is True for row in rows
                )
            round_payload[f"{arm}_replay_evaluable"] = arm_replay_evaluable
            replays_evaluable = replays_evaluable and arm_replay_evaluable

        trace_summary_path = round_dir / "stage2_composite.trace.summary.json"
        if trace_summary_path.exists():
            trace_summary = load_json(trace_summary_path)
            round_payload["stage2_trace_summary"] = trace_summary.get("global")
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

        rounds.append(round_payload)

    absolute_summary = {
        comparison: {name: describe(values) for name, values in metrics.items()}
        for comparison, metrics in absolute_deltas.items()
    }
    relative_summary = {
        comparison: {name: describe(values) for name, values in metrics.items()}
        for comparison, metrics in relative_deltas.items()
    }
    trigger_payload = {
        trigger: dict(sorted(values.items()))
        for trigger, values in sorted(trigger_totals.items())
    }
    submitted = sum(values.get("submitted", 0) for values in trigger_payload.values())
    useful = sum(values.get("useful", 0) for values in trigger_payload.values())
    pending = sum(values.get("pending_hit", 0) for values in trigger_payload.values())
    wasted = sum(values.get("wasted", 0) for values in trigger_payload.values())
    waste_rate = ratio(wasted, submitted)
    useful_rate = ratio(useful, submitted)
    pending_share_of_useful = ratio(pending, useful)

    latency_ok = all(
        median(relative_summary, f"stage2_vs_{baseline}", "latency_p95_ms") is not None
        and median(relative_summary, f"stage2_vs_{baseline}", "latency_p95_ms") <= -0.05
        and median(relative_summary, f"stage2_vs_{baseline}", "latency_p99_ms") is not None
        and median(relative_summary, f"stage2_vs_{baseline}", "latency_p99_ms") <= -0.08
        for baseline in BASELINES
    )
    recall_ok = all(
        median(absolute_summary, f"stage2_vs_{baseline}", "recall_at_k") is not None
        and median(absolute_summary, f"stage2_vs_{baseline}", "recall_at_k") >= -0.001
        for baseline in BASELINES
    )
    submitted_reads_delta = median(
        relative_summary, "stage2_vs_no_prefetch", "submitted_reads"
    )
    read_amplification_ok = (
        submitted_reads_delta is not None and submitted_reads_delta <= 0.10
    )
    waste_ok = waste_rate is not None and waste_rate <= 0.30
    current_top_k = max(stage2_submit_top_ks, default=4)
    if waste_rate is not None and waste_rate > 0.35:
        top_k_recommendation = max(2, current_top_k - 1)
        tuning_action = "reduce_post_expand_top_k"
    elif waste_ok:
        top_k_recommendation = current_top_k
        tuning_action = "lock_current_top_k"
    else:
        top_k_recommendation = current_top_k
        tuning_action = "repeat_smoke_before_locking"
    timing_issue = (
        useful_rate is not None
        and useful_rate >= 0.65
        and pending_share_of_useful is not None
        and pending_share_of_useful >= 0.50
    )

    gates = {
        "at_least_five_rounds": len(rounds) >= 5,
        "trace_validations_ok": validations_ok and bool(rounds),
        "latency_ok": latency_ok,
        "recall_ok": recall_ok,
        "waste_rate_ok": waste_ok,
        "read_amplification_ok": read_amplification_ok,
    }
    return {
        "cache_condition": cache_dir.name,
        "round_count": len(rounds),
        "rounds": rounds,
        "paired_absolute_delta_summary": absolute_summary,
        "paired_relative_delta_summary": relative_summary,
        "stage2_trigger_totals": trigger_payload,
        "stage2_rates": {
            "waste_rate": waste_rate,
            "useful_rate": useful_rate,
            "pending_share_of_useful": pending_share_of_useful,
        },
        "all_trace_validations_ok": validations_ok and bool(rounds),
        "all_replays_evaluable": replays_evaluable and bool(rounds),
        "timing_issue_without_width_increase": timing_issue,
        "tuning": {
            "current_post_expand_top_k": current_top_k,
            "recommended_post_expand_top_k": top_k_recommendation,
            "action": tuning_action,
        },
        "acceptance_gates": gates,
        "acceptance_ok": all(gates.values()),
    }


def render_markdown(payload: dict) -> str:
    lines = [
        "# Stage 2 Prefetch A/B Report",
        "",
        f"Suite: `{payload['suite_id']}`",
        "",
        f"Overall acceptance: `{payload['acceptance_ok']}`",
        "",
    ]
    for cache_name, summary in sorted(payload["cache_conditions"].items()):
        rates = summary["stage2_rates"]
        lines.extend(
            [
                f"## {cache_name}",
                "",
                f"- Paired rounds: {summary['round_count']}",
                f"- Acceptance: {summary['acceptance_ok']}",
                f"- Trace validation: {summary['all_trace_validations_ok']}",
                f"- Waste rate: {rates['waste_rate']}",
                f"- Useful rate: {rates['useful_rate']}",
                f"- Pending/useful: {rates['pending_share_of_useful']}",
                "- Tuning: "
                f"{summary['tuning']['action']} -> top-K "
                f"{summary['tuning']['recommended_post_expand_top_k']}",
                "",
                "| Comparison | Metric | Median Relative Delta | Median Absolute Delta |",
                "| --- | --- | ---: | ---: |",
            ]
        )
        for comparison in ("stage2_vs_no_prefetch", "stage2_vs_stage1_prebeam"):
            for metric in ("latency_p95_ms", "latency_p99_ms", "recall_at_k", "submitted_reads"):
                relative = median(summary["paired_relative_delta_summary"], comparison, metric)
                absolute = median(summary["paired_absolute_delta_summary"], comparison, metric)
                lines.append(f"| `{comparison}` | `{metric}` | {relative} | {absolute} |")
        lines.extend(["", "Acceptance gates:", ""])
        for name, passed in summary["acceptance_gates"].items():
            lines.append(f"- `{name}`: {passed}")
        lines.append("")
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description="Summarize Stage 2 three-arm runs.")
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
    required_cache_conditions = {"cold", "hot"}
    payload["required_cache_conditions_present"] = (
        required_cache_conditions <= set(payload["cache_conditions"])
    )
    payload["acceptance_ok"] = payload["required_cache_conditions_present"] and all(
        payload["cache_conditions"][name]["acceptance_ok"]
        for name in required_cache_conditions
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
