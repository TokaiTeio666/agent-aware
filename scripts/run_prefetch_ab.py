#!/usr/bin/env python3
"""Run no-prefetch vs XGBoost prefetch A/B experiments."""

from __future__ import annotations

import argparse
import json
import math
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional


def parse_extra_args(value: str) -> List[str]:
    return shlex.split(value) if value else []


def run_command(command: List[str], cwd: Path, stdout_path: Path, stderr_path: Path) -> None:
    stdout_path.parent.mkdir(parents=True, exist_ok=True)
    completed = subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    stdout_path.write_text(completed.stdout, encoding="utf-8")
    stderr_path.write_text(completed.stderr, encoding="utf-8")
    if completed.returncode != 0:
        raise RuntimeError(
            f"command failed with exit code {completed.returncode}: {shlex.join(command)}"
        )


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def nested_get(payload: dict, *names: str) -> Any:
    for name in names:
        current: Any = payload
        ok = True
        for part in name.split("."):
            if isinstance(current, dict) and part in current:
                current = current[part]
            else:
                ok = False
                break
        if ok:
            return current
    return None


def percentile(values: List[float], pct: float) -> Optional[float]:
    if not values:
        return None
    ordered = sorted(values)
    rank = math.ceil((pct / 100.0) * len(ordered))
    rank = min(max(rank, 1), len(ordered))
    return ordered[rank - 1]


def result_metrics(path: Path) -> dict:
    data = read_json(path)
    latencies = nested_get(data, "query_latency_ms") or []
    return {
        "result_json": str(path),
        "recall@10": nested_get(data, "recall_at_k", "recall@10"),
        "qps": nested_get(data, "qps"),
        "avg_latency_ms": nested_get(data, "avg_query_latency_ms", "avg_latency_ms"),
        "p50_latency_ms": nested_get(data, "latency_p50_ms")
        if nested_get(data, "latency_p50_ms") is not None
        else percentile([float(v) for v in latencies], 50.0),
        "p95_latency_ms": nested_get(data, "latency_p95_ms", "p95_latency_ms"),
        "p99_latency_ms": nested_get(data, "latency_p99_ms", "p99_latency_ms"),
        "submitted_reads": nested_get(data, "stats.submitted_reads", "submitted_reads"),
        "demand_reads": nested_get(data, "stats.demand_reads", "demand_reads"),
        "prefetch_reads": nested_get(data, "stats.prefetch_reads", "prefetch_reads"),
        "cache_hits": nested_get(data, "stats.cache_hits", "cache_hits"),
        "cache_misses": nested_get(data, "stats.page_cache_misses", "cache_misses"),
        "raw_status": data.get("status"),
        "stats": data.get("stats", {}),
    }


def trace_metrics(path: Optional[Path]) -> dict:
    if path is None or not path.exists():
        return {
            "ready_hit_rate": None,
            "pending_hit_rate": None,
            "useful_rate": None,
            "waste_rate": None,
            "drop_rate": None,
            "avg_lead_time_us": None,
            "p95_lead_time_us": None,
        }
    summary = read_json(path)
    global_summary = summary.get("global", summary)
    return {
        key: global_summary.get(key)
        for key in [
            "ready_hit_rate",
            "pending_hit_rate",
            "useful_rate",
            "waste_rate",
            "drop_rate",
            "avg_lead_time_us",
            "p95_lead_time_us",
        ]
    }


def percent_delta(new: Optional[float], old: Optional[float]) -> Optional[float]:
    if new is None or old in (None, 0):
        return None
    return (new - old) / old


def recall_not_down(candidate: dict, baseline: dict, tolerance: float = 1e-9) -> bool:
    cand = candidate.get("recall@10")
    base = baseline.get("recall@10")
    if cand is None or base is None:
        return True
    return float(cand) + tolerance >= float(base)


def conclusions(summary: dict) -> List[str]:
    rows = summary["runs"]
    no_prefetch = rows["noprefetch"]["result"]
    xgboost = rows["xgboost"]["result"]
    xgboost_trace = rows["xgboost"]["trace"]
    output = []

    p95_delta = percent_delta(xgboost.get("p95_latency_ms"), no_prefetch.get("p95_latency_ms"))
    p99_delta = percent_delta(xgboost.get("p99_latency_ms"), no_prefetch.get("p99_latency_ms"))
    if (
        ((p95_delta is not None and p95_delta <= -0.05) or (p99_delta is not None and p99_delta <= -0.05))
        and recall_not_down(xgboost, no_prefetch)
    ):
        output.append("xgboost 相比 no-prefetch 初步有效：p95 或 p99 降低 >= 5%，且 recall 未下降。")
    elif p95_delta is not None and p95_delta > 0:
        output.append("xgboost 相比 no-prefetch 可能负优化：p95 上升。")
    else:
        output.append("xgboost 尚未证明优于 no-prefetch：尾延迟改善不足 5% 或 recall 有下降风险。")

    ready = xgboost_trace.get("ready_hit_rate") or 0.0
    pending = xgboost_trace.get("pending_hit_rate") or 0.0
    waste = xgboost_trace.get("waste_rate") or 0.0
    if pending > ready:
        output.append("xgboost 的 pending_hit_rate 高于 ready_hit_rate：主要问题像是触发太晚。")
    if waste >= 0.3:
        output.append("xgboost waste_rate 较高：模型排序、threshold 或 max-inflight 需要收紧。")

    xgb_reads = xgboost.get("submitted_reads") or 0
    base_reads = no_prefetch.get("submitted_reads") or 0
    if base_reads and xgb_reads > base_reads * 1.15 and not (
        p95_delta is not None and p95_delta < 0
    ):
        output.append("xgboost 可能有 I/O 放大：submitted_reads 增加但尾延迟未下降。")
    return output


def make_markdown(summary: dict) -> str:
    lines = [
        "# Prefetch A/B Summary",
        "",
        "## Result Metrics",
        "",
        "| Run | Recall@10 | QPS | Avg ms | P50 ms | P95 ms | P99 ms | Submitted Reads | Demand Reads | Prefetch Reads | Cache Hits | Cache Misses |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]

    def fmt(value):
        if value is None:
            return "null"
        if isinstance(value, float):
            return f"{value:.6g}"
        return str(value)

    for name, payload in summary["runs"].items():
        row = payload["result"]
        lines.append(
            "| {name} | {recall} | {qps} | {avg} | {p50} | {p95} | {p99} | {submitted} | {demand} | {prefetch} | {hits} | {misses} |".format(
                name=name,
                recall=fmt(row.get("recall@10")),
                qps=fmt(row.get("qps")),
                avg=fmt(row.get("avg_latency_ms")),
                p50=fmt(row.get("p50_latency_ms")),
                p95=fmt(row.get("p95_latency_ms")),
                p99=fmt(row.get("p99_latency_ms")),
                submitted=fmt(row.get("submitted_reads")),
                demand=fmt(row.get("demand_reads")),
                prefetch=fmt(row.get("prefetch_reads")),
                hits=fmt(row.get("cache_hits")),
                misses=fmt(row.get("cache_misses")),
            )
        )

    lines.extend(
        [
            "",
            "## Trace Metrics",
            "",
            "| Run | Ready Hit | Pending Hit | Useful | Waste | Drop | Avg Lead us | P95 Lead us |",
            "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    row = summary["runs"]["xgboost"]["trace"]
    lines.append(
        "| xgboost | {ready} | {pending} | {useful} | {waste} | {drop} | {avg} | {p95} |".format(
            ready=fmt(row.get("ready_hit_rate")),
            pending=fmt(row.get("pending_hit_rate")),
            useful=fmt(row.get("useful_rate")),
            waste=fmt(row.get("waste_rate")),
            drop=fmt(row.get("drop_rate")),
            avg=fmt(row.get("avg_lead_time_us")),
            p95=fmt(row.get("p95_lead_time_us")),
        )
    )

    lines.extend(["", "## Conclusions", ""])
    for item in summary["conclusions"]:
        lines.append(f"- {item}")
    return "\n".join(lines) + "\n"


def build_base_command(args: argparse.Namespace, output_json: Path) -> List[str]:
    command = [
        str(args.binary),
        "--search-width",
        str(args.search_width),
        "--beam-width",
        str(args.beam_width),
        "--entry-count",
        str(args.entry_count),
        "--enable-pq",
        "1",
        "--io-mode",
        "io_uring",
        "--cache-policy",
        "graph-aware-2q",
    ]
    if args.query_limit is not None:
        command.extend(["--query-limit", str(args.query_limit)])
    if args.base_limit is not None:
        command.extend(["--base-limit", str(args.base_limit)])
    command.extend(args.extra_args)
    command.extend(["--output-json", str(output_json)])
    return command


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description="Run no-prefetch vs XGBoost prefetch A/B.")
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--search-width", type=int, default=350)
    parser.add_argument("--beam-width", type=int, default=16)
    parser.add_argument("--entry-count", type=int, default=64)
    parser.add_argument("--prefetch-top-k", type=int, default=4)
    parser.add_argument("--prefetch-max-inflight", type=int, default=32)
    parser.add_argument("--prefetch-score-threshold", type=float, default=0.2)
    parser.add_argument("--query-limit", type=int)
    parser.add_argument("--base-limit", type=int)
    parser.add_argument("--extra-args", type=parse_extra_args, default=[])
    args = parser.parse_args()

    args.binary = args.binary.resolve()
    args.model = args.model.resolve()
    args.out_dir = args.out_dir.resolve()
    if not args.binary.exists():
        raise SystemExit(f"missing binary: {args.binary}")
    if not args.model.exists():
        raise SystemExit(f"missing model: {args.model}")
    args.out_dir.mkdir(parents=True, exist_ok=True)

    run_specs = {
        "noprefetch": ["--enable-prefetch", "0", "--prefetch-policy", "none"],
        "xgboost": [
            "--enable-prefetch",
            "1",
            "--prefetch-policy",
            "xgboost",
            "--prefetch-model",
            str(args.model),
            "--prefetch-top-k",
            str(args.prefetch_top_k),
            "--prefetch-max-inflight",
            str(args.prefetch_max_inflight),
            "--prefetch-score-threshold",
            str(args.prefetch_score_threshold),
        ],
    }

    trace_summaries: Dict[str, Optional[Path]] = {"noprefetch": None}
    for name, variant_args in run_specs.items():
        run_dir = args.out_dir / name
        run_dir.mkdir(parents=True, exist_ok=True)
        result_json = run_dir / "result.json"
        command = build_base_command(args, result_json)
        if name == "xgboost":
            trace = run_dir / "trace.csv"
            variant_args = [*variant_args, "--prefetch-trace", str(trace)]
        command.extend(variant_args)
        (run_dir / "command.txt").write_text(shlex.join(command) + "\n", encoding="utf-8")
        run_command(command, root, run_dir / "stdout.log", run_dir / "stderr.log")

        if name == "xgboost":
            trace_summary_json = run_dir / "trace_summary.json"
            trace_summary_md = run_dir / "trace_summary.md"
            analyze_command = [
                sys.executable,
                str(root / "scripts" / "analyze_prefetch_trace.py"),
                "--trace",
                str(run_dir / "trace.csv"),
                "--json-out",
                str(trace_summary_json),
                "--md-out",
                str(trace_summary_md),
                "--group-by",
                "trigger",
            ]
            run_command(
                analyze_command,
                root,
                run_dir / "analyze_stdout.log",
                run_dir / "analyze_stderr.log",
            )
            trace_summaries[name] = trace_summary_json

    summary = {
        "out_dir": str(args.out_dir),
        "binary": str(args.binary),
        "model": str(args.model),
        "parameters": {
            "search_width": args.search_width,
            "beam_width": args.beam_width,
            "entry_count": args.entry_count,
            "prefetch_top_k": args.prefetch_top_k,
            "prefetch_max_inflight": args.prefetch_max_inflight,
            "prefetch_score_threshold": args.prefetch_score_threshold,
            "query_limit": args.query_limit,
            "base_limit": args.base_limit,
            "extra_args": args.extra_args,
        },
        "runs": {},
    }
    for name in run_specs:
        result = result_metrics(args.out_dir / name / "result.json")
        summary["runs"][name] = {
            "result": result,
            "trace": trace_metrics(trace_summaries.get(name)),
        }
    summary["conclusions"] = conclusions(summary)

    (args.out_dir / "ab_summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    (args.out_dir / "ab_summary.md").write_text(make_markdown(summary), encoding="utf-8")
    print(json.dumps(summary, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
