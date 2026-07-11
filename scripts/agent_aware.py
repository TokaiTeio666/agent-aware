#!/usr/bin/env python3
from __future__ import annotations

import argparse
import filecmp
import json
import os
import shlex
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
AB_ARMS = ("no_prefetch", "topology_fixed", "topology_adaptive")


def run(command: list[str], *, output: Path | None, dry_run: bool) -> None:
    print(shlex.join(command))
    if dry_run:
        return
    if output is None:
        subprocess.run(command, cwd=ROOT, check=True)
        return
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with output.open("w", encoding="utf-8") as stream:
            subprocess.run(
                command, cwd=ROOT, stdout=stream,
                stderr=subprocess.STDOUT, check=True,
            )
    except subprocess.CalledProcessError as error:
        tail = output.read_text(encoding="utf-8", errors="replace").splitlines()[-40:]
        detail = "\n".join(tail)
        raise SystemExit(
            f"command failed with exit code {error.returncode}; log: {output}\n{detail}"
        ) from error


def resolve_index_manifest(index: Path, *, dry_run: bool) -> Path | None:
    if dry_run:
        return None
    resolved = index if index.is_absolute() else ROOT / index
    if not resolved.exists():
        raise SystemExit(f"missing packed index: {resolved}; run agent-aware-build first")
    manifest = Path(str(resolved) + ".manifest.json")
    if manifest.exists():
        return manifest
    candidates = sorted(resolved.parent.glob("*.idx.manifest.json"))
    for candidate in candidates:
        candidate_index = Path(str(candidate)[: -len(".manifest.json")])
        if (candidate_index.exists() and
                candidate_index.stat().st_size == resolved.stat().st_size and
                filecmp.cmp(candidate_index, resolved, shallow=False)):
            print(f"using manifest from identical index artifact: {candidate}")
            return candidate
    hint = ""
    if candidates:
        hint = "\navailable manifests:\n" + "\n".join(
            f"  {candidate}" for candidate in candidates
        )
    raise SystemExit(
        f"missing build manifest: {manifest}; rebuild the index or pass a complete index artifact{hint}"
    )


def once(args: argparse.Namespace) -> None:
    if not args.no_build:
        run(
            [
                "cmake", "--build", str(args.build_dir), "--target",
                "agent-aware-search", "-j", str(args.jobs),
            ],
            output=None,
            dry_run=args.dry_run,
        )
    manifest = resolve_index_manifest(args.index, dry_run=args.dry_run)

    output_dir = args.output_dir or (
        ROOT / "logs" / "sift_bench" / time.strftime("once_%Y%m%d-%H%M%S")
    )
    result = output_dir / "result.json"
    command = [
        str(args.binary), "--index-path", str(args.index),
        *([] if manifest is None else ["--manifest", str(manifest)]),
        "--output-json", str(result), *args.extra_arg,
    ]
    started = time.time()
    run(command, output=output_dir / "run.log", dry_run=args.dry_run)
    if args.dry_run:
        return
    payload = json.loads(result.read_text(encoding="utf-8"))
    payload["runner_wall_seconds"] = time.time() - started
    print(json.dumps(payload, indent=2, sort_keys=True))


def topology_args(arm: str, trace: Path) -> list[str]:
    if arm == "no_prefetch":
        return ["--enable-prefetch", "0", "--prefetch-policy", "none"]
    return [
        "--enable-prefetch", "1", "--prefetch-policy", "topology",
        "--prefetch-adaptive", "1" if arm == "topology_adaptive" else "0",
        "--prefetch-top-k", "2", "--prefetch-max-inflight", "2",
        "--prefetch-trace", str(trace),
    ]


def ab(args: argparse.Namespace) -> None:
    if not args.no_build:
        run(["cmake", "--build", str(args.build_dir), "--target",
             "agent-aware-search", "-j", str(args.jobs)],
            output=None, dry_run=args.dry_run)
    manifest = resolve_index_manifest(args.index, dry_run=args.dry_run)
    output_root = args.output_root or ROOT / "logs" / "prefetch_ab" / time.strftime("ab_%Y%m%d-%H%M%S")
    for cache in (item.strip() for item in args.cache_conditions.split(",")):
        for round_index, seed_text in enumerate(args.seeds.split(","), 1):
            seed = int(seed_text)
            round_dir = output_root / cache / f"round-{round_index:02d}"
            for arm in AB_ARMS:
                result = round_dir / f"{arm}.json"
                trace = round_dir / f"{arm}.trace.csv"
                command = [
                    str(args.binary), "--index-path", str(args.index),
                    *([] if manifest is None else ["--manifest", str(manifest)]),
                    "--query-offset", str(args.query_offset),
                    "--query-limit", str(args.query_limit),
                    "--search-width", str(args.search_width),
                    "--beam-width", str(args.beam_width),
                    "--io-mode", args.io_mode, "--io-depth", str(args.io_depth),
                    "--io-batch", str(args.io_batch),
                    "--cache-pages", str(args.cache_pages),
                    "--cache-condition", cache, "--seed", str(seed),
                    "--run-id", f"topology-ab:{cache}:{round_index}:{arm}",
                    "--policy-version", arm, "--output-json", str(result),
                    *topology_args(arm, trace), *args.extra_arg,
                ]
                if cache == "hot":
                    command.extend(["--warmup-query-limit", str(args.query_limit)])
                run(command, output=round_dir / f"{arm}.log",
                    dry_run=args.dry_run)
    if not args.dry_run:
        run([sys.executable, str(ROOT / "scripts" / "summarize_prefetch_ab.py"),
             "--root", str(output_root)], output=None, dry_run=False)


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description="Agent-Aware benchmark runner")
    commands = root.add_subparsers(dest="command", required=True)
    for name, handler in (("once", once), ("ab", ab)):
        command = commands.add_parser(name)
        command.set_defaults(handler=handler)
        command.add_argument("--binary", type=Path, default=ROOT / "build" / "agent-aware-search")
        command.add_argument("--build-dir", type=Path, default=ROOT / "build")
        command.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
        command.add_argument("--no-build", action="store_true")
        command.add_argument("--dry-run", action="store_true")
        command.add_argument("--extra-arg", action="append", default=[])
        command.add_argument("--index", type=Path, default=ROOT / "indexes" / "sift.idx")
        if name == "once":
            command.add_argument("--output-dir", type=Path)
        else:
            command.add_argument("--output-root", type=Path)
            command.add_argument("--seeds", default="42,43,44,45,46")
            command.add_argument("--cache-conditions", default="cold,hot")
            command.add_argument("--query-offset", type=int, default=5000)
            command.add_argument("--query-limit", type=int, default=1000)
            command.add_argument("--search-width", type=int, default=156)
            command.add_argument("--beam-width", type=int, default=16)
            command.add_argument("--io-mode", default="io_uring")
            command.add_argument("--io-depth", type=int, default=32)
            command.add_argument("--io-batch", type=int, default=16)
            command.add_argument("--cache-pages", type=int, default=15000)
    return root


def main() -> None:
    args = parser().parse_args()
    args.handler(args)


if __name__ == "__main__":
    main()
