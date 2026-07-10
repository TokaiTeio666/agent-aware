#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UTILITY_COMMANDS = {
    "analyze": "prefetch_trace.py",
    "validate": "prefetch_trace.py",
    "train": "train_prefetch_ranker.py",
    "replay": "replay_prefetch_policy.py",
    "summarize": "summarize_prefetch_ab.py",
}
UTILITY_HELP = {
    "analyze": "summarize a prefetch trace",
    "validate": "validate trace and result contracts",
    "train": "train the prefetch ranker",
    "replay": "replay a prefetch policy",
    "summarize": "summarize A/B benchmark results",
}


def run(command: list[str], *, output: Path | None = None, dry_run: bool = False) -> None:
    print(shlex.join(command))
    if dry_run:
        return
    if output is None:
        subprocess.run(command, cwd=ROOT, check=True)
        return
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8") as stream:
        subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT, check=True)


def build(args: argparse.Namespace) -> None:
    if not args.no_build:
        run(["cmake", "--build", str(args.build_dir), "--target", "agent-aware",
             "-j", str(args.jobs)], dry_run=args.dry_run)


def common_search(args: argparse.Namespace) -> list[str]:
    return [
        "--index-path", str(args.index), "--top-k", "10",
        "--search-width", str(args.search_width), "--beam-width", str(args.beam_width),
        "--entry-count", str(args.entry_count), "--rerank-topk", str(args.rerank_topk),
        "--enable-pq", "1", "--pq-subspaces", "32", "--pq-centroids", "256",
        "--pq-train-limit", "100000", "--pq-iterations", "8",
        "--io-mode", args.io_mode, "--io-depth", str(args.io_depth),
        "--io-batch", str(args.io_batch), "--cache-pages", str(args.cache_pages),
        "--memory-budget-ratio", str(args.memory_budget),
        "--query-offset", str(args.query_offset), "--query-limit", str(args.query_limit),
        "--rebuild-index", "0", *args.extra_arg,
    ]


def once(args: argparse.Namespace) -> None:
    build(args)
    output_dir = args.output_dir or ROOT / "logs" / "sift_bench" / time.strftime("once_%Y%m%d-%H%M%S")
    result = output_dir / "result.json"
    command = [str(args.binary), "--output-json", str(result), *args.extra_arg]
    started = time.time()
    run(command, output=output_dir / "run.log", dry_run=args.dry_run)
    if args.dry_run:
        return
    payload = json.loads(result.read_text(encoding="utf-8"))
    payload["runner_wall_seconds"] = time.time() - started
    print(json.dumps(payload, indent=2, sort_keys=True))


def arm_args(args: argparse.Namespace, arm: str, trace: Path) -> list[str]:
    if arm == "no_prefetch":
        return ["--enable-prefetch", "0", "--prefetch-policy", "none"]
    base = ["--enable-prefetch", "1", "--prefetch-policy", "xgboost",
            "--prefetch-model", str(args.model.resolve()), "--prefetch-trace", str(trace),
            "--prefetch-max-inflight", "8"]
    if arm == "stage1_prebeam":
        return [*base, "--prefetch-width", "1", "--prefetch-top-k", "1",
                "--prefetch-submit-top-k", "0", "--prefetch-early-trigger", "pre-beam"]
    return [*base, "--prefetch-width", "2", "--prefetch-top-k", "2",
            "--prefetch-submit-top-k", str(args.post_expand_top_k),
            "--prefetch-early-trigger", "all", "--jit-prefetch-interval-batches", "1",
            "--jit-prefetch-max-pages-per-query", "16"]


def postprocess(args: argparse.Namespace, result: Path, trace: Path) -> None:
    prefix = result.parent / result.stem
    run([sys.executable, str(Path(__file__)), "analyze", "--trace", str(trace),
         "--json-out", str(prefix) + ".trace.summary.json"], dry_run=args.dry_run)
    run([sys.executable, str(Path(__file__)), "validate", "--trace", str(trace),
         "--result-json", str(result), "--json-out", str(prefix) + ".trace.validation.json"],
        dry_run=args.dry_run)
    run([sys.executable, str(Path(__file__)), "replay", "--trace", str(trace),
         "--model", str(args.model), "--top-k", str(args.post_expand_top_k),
         "--match-online-budget", "--out-json", str(prefix) + ".replay.json"],
        dry_run=args.dry_run)


def ab(args: argparse.Namespace) -> None:
    build(args)
    if not args.model.exists() and not args.dry_run:
        raise SystemExit(f"missing prefetch model: {args.model}")
    output_root = args.output_root or ROOT / "build" / f"prefetch_ab_{time.strftime('%Y%m%d-%H%M%S')}"
    seeds = [int(value) for value in args.seeds.split(",")]
    arms = ["no_prefetch", "stage1_prebeam", "stage2_composite"]
    for cache in args.cache_conditions.split(","):
        for round_index, seed in enumerate(seeds, 1):
            round_dir = output_root / cache / f"round-{round_index:02d}"
            order = arms[(round_index - 1) % 3:] + arms[:(round_index - 1) % 3]
            if not args.dry_run:
                round_dir.mkdir(parents=True, exist_ok=True)
                (round_dir / "order.txt").write_text(" ".join(order) + "\n", encoding="utf-8")
            for arm in order:
                if cache == "cold":
                    if not args.cold_reset:
                        raise SystemExit("--cold-reset is required for cold runs")
                    run(shlex.split(args.cold_reset), dry_run=args.dry_run)
                result, trace = round_dir / f"{arm}.json", round_dir / f"{arm}.trace.csv"
                command = [str(args.binary), *common_search(args), "--seed", str(seed),
                           "--run-id", f"ab:{cache}:{round_index}:{arm}:{seed}",
                           "--policy-version", arm, "--cache-condition", cache,
                           "--output-json", str(result), *arm_args(args, arm, trace)]
                if cache == "hot":
                    command.extend(["--warmup-query-limit", str(args.query_limit)])
                run(command, output=round_dir / f"{arm}.log", dry_run=args.dry_run)
                if arm != "no_prefetch":
                    postprocess(args, result, trace)
    run([sys.executable, str(Path(__file__)), "summarize", "--root", str(output_root),
         "--json-out", str(output_root / "report.json"),
         "--md-out", str(output_root / "report.md")], dry_run=args.dry_run)


def tune(args: argparse.Namespace) -> None:
    build(args)
    output_root = args.output_root or ROOT / "build" / f"stage2_tuning_{time.strftime('%Y%m%d-%H%M%S')}"
    selected, status = 2, "exhausted"
    for top_k in (4, 3, 2):
        args.post_expand_top_k = top_k
        run_dir = output_root / f"topk-{top_k}"
        result, trace = run_dir / "result.json", run_dir / "trace.csv"
        command = [str(args.binary), *common_search(args), "--seed", str(args.seed),
                   "--output-json", str(result), *arm_args(args, "stage2_composite", trace)]
        run(command, output=run_dir / "run.log", dry_run=args.dry_run)
        postprocess(args, result, trace)
        selected = top_k
        if args.dry_run:
            continue
        summary = json.loads((run_dir / "result.trace.summary.json").read_text(encoding="utf-8"))
        if summary["global"]["waste_rate"] <= args.waste_threshold:
            status = "accepted"
            break
    if not args.dry_run:
        output_root.mkdir(parents=True, exist_ok=True)
        (output_root / "selection.json").write_text(
            json.dumps({"selected_post_expand_top_k": selected, "status": status,
                        "continue_tuning_threshold": args.waste_threshold}, indent=2) + "\n",
            encoding="utf-8")


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(
        description="Agent-Aware benchmark and prefetch tools",
    )
    commands = root.add_subparsers(dest="command", required=True)
    for name, help_text in UTILITY_HELP.items():
        commands.add_parser(name, add_help=False, help=help_text)
    for name, handler in (("once", once), ("ab", ab), ("tune", tune)):
        item = commands.add_parser(name)
        item.set_defaults(handler=handler)
        item.add_argument("--binary", type=Path, default=ROOT / "build" / "agent-aware")
        item.add_argument("--build-dir", type=Path, default=ROOT / "build")
        item.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
        item.add_argument("--no-build", action="store_true")
        item.add_argument("--dry-run", action="store_true")
        item.add_argument("--output-root", type=Path)
        item.add_argument("--extra-arg", action="append", default=[])
        if name == "once":
            item.add_argument("--output-dir", type=Path)
            continue
        item.add_argument("--index", type=Path, default=ROOT / "indexes" / "sift.idx")
        item.add_argument("--model", type=Path, default=ROOT / "build" / "xgboost_model.txt")
        item.add_argument("--query-offset", type=int, default=0 if name == "ab" else 5000)
        item.add_argument("--query-limit", type=int, default=1000 if name == "ab" else 200)
        item.add_argument("--search-width", type=int, default=350)
        item.add_argument("--beam-width", type=int, default=16)
        item.add_argument("--entry-count", type=int, default=64)
        item.add_argument("--rerank-topk", type=int, default=100)
        item.add_argument("--io-mode", default="io_uring")
        item.add_argument("--io-depth", type=int, default=64)
        item.add_argument("--io-batch", type=int, default=16)
        item.add_argument("--cache-pages", type=int, default=15000)
        item.add_argument("--memory-budget", type=float, default=0.20)
        item.add_argument("--post-expand-top-k", type=int, default=4)
        if name == "ab":
            item.add_argument("--seeds", default="42,43,44,45,46")
            item.add_argument("--cache-conditions", default="cold,hot")
            item.add_argument("--cold-reset")
        else:
            item.add_argument("--seed", type=int, default=42)
            item.add_argument("--waste-threshold", type=float, default=0.35)
    return root


def main() -> None:
    if len(sys.argv) > 1 and sys.argv[1] in UTILITY_COMMANDS:
        script = Path(__file__).with_name(UTILITY_COMMANDS[sys.argv[1]])
        prefix = [sys.argv[1]] if sys.argv[1] in {"analyze", "validate"} else []
        raise SystemExit(subprocess.call([sys.executable, str(script), *prefix, *sys.argv[2:]], cwd=ROOT))
    args = parser().parse_args()
    args.handler(args)


if __name__ == "__main__":
    main()
