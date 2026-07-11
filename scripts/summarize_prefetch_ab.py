#!/usr/bin/env python3
import argparse
import json
import statistics
from pathlib import Path

ARMS = ("no_prefetch", "topology_fixed", "topology_adaptive")
METRICS = ("latency_p95_ms", "latency_p99_ms", "recall_at_k", "qps")


def value(payload, name):
    return payload.get(name, payload.get("stats", {}).get(name))


def median_delta(rounds, baseline, metric, relative=True):
    deltas = []
    for arms in rounds:
        candidate, base = value(arms["topology_adaptive"], metric), value(arms[baseline], metric)
        if candidate is None or base is None or (relative and base == 0):
            continue
        deltas.append((candidate - base) / base if relative else candidate - base)
    return statistics.median(deltas) if deltas else None


def summarize(cache_dir):
    rounds = []
    for directory in sorted(cache_dir.glob("round-*")):
        paths = {arm: directory / f"{arm}.json" for arm in ARMS}
        if all(path.exists() for path in paths.values()):
            rounds.append({arm: json.loads(path.read_text(encoding="utf-8")) for arm, path in paths.items()})
    stats = [arm["topology_adaptive"]["stats"] for arm in rounds]
    submitted = sum(item["prefetch_submitted"] for item in stats)
    ready = sum(item["prefetch_ready_hit"] for item in stats)
    pending = sum(item["prefetch_pending_hit"] for item in stats)
    useful = sum(item["prefetch_useful_pages"] for item in stats)
    wasted = sum(item["prefetch_wasted_pages"] for item in stats)
    ratio = lambda a, b: a / b if b else None
    p95 = median_delta(rounds, "no_prefetch", "latency_p95_ms")
    p99 = median_delta(rounds, "no_prefetch", "latency_p99_ms")
    recall = median_delta(rounds, "no_prefetch", "recall_at_k", False)
    reads = median_delta(rounds, "no_prefetch", "submitted_reads")
    syscalls = median_delta(rounds, "no_prefetch", "io_submit_syscalls")
    demand_wait = median_delta(rounds, "no_prefetch", "demand_read_wait_us")
    planner_cpu = [value(arm["topology_adaptive"], "prefetch_planner_cpu_ratio") for arm in rounds]
    planner_cpu = statistics.median([item for item in planner_cpu if item is not None]) if planner_cpu else None
    gates = {
        "at_least_five_rounds": len(rounds) >= 5,
        "p95_improved_5pct": p95 is not None and p95 <= -0.05,
        "p99_improved_8pct": p99 is not None and p99 <= -0.08,
        "recall_drop_within_0_001": recall is not None and recall >= -0.001,
        "read_amplification_within_3pct": reads is not None and reads <= 0.03,
        "submit_syscalls_within_5pct": syscalls is not None and syscalls <= 0.05,
        "demand_wait_improved": demand_wait is not None and demand_wait < 0,
        "ready_rate_at_least_50pct": ratio(ready, submitted) is not None and ratio(ready, submitted) >= 0.50,
        "waste_rate_at_most_15pct": ratio(wasted, submitted) is not None and ratio(wasted, submitted) <= 0.15,
        "pending_share_at_most_35pct": ratio(pending, useful) is not None and ratio(pending, useful) <= 0.35,
        "planner_cpu_at_most_5pct": planner_cpu is not None and planner_cpu <= 0.05,
    }
    comparisons = {}
    for baseline in ("no_prefetch", "topology_fixed"):
        comparisons[f"topology_adaptive_vs_{baseline}"] = {
            metric: median_delta(rounds, baseline, metric,
                                 metric != "recall_at_k")
            for metric in ("latency_p95_ms", "latency_p99_ms", "recall_at_k",
                           "submitted_reads", "io_submit_syscalls",
                           "demand_read_wait_us")
        }
    return {"round_count": len(rounds), "comparisons": comparisons,
        "deltas_vs_no_prefetch": {
        "p95": p95, "p99": p99, "recall": recall, "submitted_reads": reads,
        "io_submit_syscalls": syscalls, "demand_read_wait_us": demand_wait},
        "rates": {"ready": ratio(ready, submitted), "waste": ratio(wasted, submitted),
                  "pending_share": ratio(pending, useful), "planner_cpu_ratio": planner_cpu},
        "gates": gates, "acceptance_ok": all(gates.values())}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    args = parser.parse_args()
    payload = {"cache_conditions": {name: summarize(args.root / name) for name in ("cold", "hot")}}
    payload["acceptance_ok"] = all(item["acceptance_ok"] for item in payload["cache_conditions"].values())
    args.root.mkdir(parents=True, exist_ok=True)
    (args.root / "report.json").write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(payload, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
