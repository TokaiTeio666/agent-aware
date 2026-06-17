#!/usr/bin/env python3
import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path


def parse_csv_ints(value):
    output = []
    for item in value.split(","):
        item = item.strip()
        if item:
            output.append(int(item))
    return output


def git_lines(root, args):
    try:
        completed = subprocess.run(
            ["git", *args],
            cwd=root,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    except OSError:
        return []
    return [line for line in completed.stdout.splitlines() if line]


def binary_metadata(root, binary):
    try:
        binary_mtime = int(binary.stat().st_mtime)
    except OSError:
        binary_mtime = None
    commit_lines = git_lines(root, ["rev-parse", "HEAD"])
    return {
        "binary_path": str(binary),
        "binary_mtime": binary_mtime,
        "git_commit": commit_lines[0] if commit_lines else "unknown",
        "dirty_untracked_files": git_lines(
            root, ["status", "--porcelain", "--untracked-files=all"]
        ),
    }


def write_json(path, payload):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")


def skip_payload(root, binary, reason, search_width, beam_width, prefetch_depth):
    payload = {
        "status": "skipped",
        "requested_search_width": search_width,
        "effective_search_width": None,
        "requested_beam_width": beam_width,
        "effective_beam_width": None,
        "beam_width_supported": None,
        "requested_prefetch_depth": prefetch_depth,
        "effective_prefetch_depth": None,
        "prefetch_enabled": False,
        "io_uring_enabled": False,
        "odirect_enabled": False,
        "warning": reason,
        "warnings": [reason],
    }
    payload.update(binary_metadata(root, binary))
    return payload


def run_help(binary):
    completed = subprocess.run(
        [str(binary), "--help"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    return completed.returncode, completed.stdout


def main():
    parser = argparse.ArgumentParser(
        description="Run honest agentmem_flow SIFT matrix experiments."
    )
    root = Path(__file__).resolve().parents[2]
    parser.add_argument("--binary", default=str(root / "build" / "agentmem_flow"))
    parser.add_argument("--sift-dir", default=str(root / "data" / "sift"))
    parser.add_argument("--output-dir", default=str(root / "logs" / "sift_bench"))
    parser.add_argument("--base-limit", type=int, default=int(os.getenv("BASE_LIMIT", "10000")))
    parser.add_argument("--query-limit", type=int, default=int(os.getenv("QUERY_LIMIT", "100")))
    parser.add_argument("--top-k", type=int, default=int(os.getenv("K", "10")))
    parser.add_argument("--entry-count", type=int, default=int(os.getenv("ENTRY_COUNT", "32")))
    parser.add_argument("--io-mode", default=os.getenv("IO_MODE", "pread"))
    parser.add_argument("--io-depth", type=int, default=int(os.getenv("IO_DEPTH", "1")))
    parser.add_argument("--io-batch", type=int, default=int(os.getenv("IO_BATCH", "1")))
    parser.add_argument("--cache-policy", default=os.getenv("CACHE_POLICY", "none"))
    parser.add_argument("--cache-pages", type=int, default=int(os.getenv("CACHE_PAGES", "0")))
    parser.add_argument("--rebuild-index", choices=["0", "1"], default=os.getenv("REBUILD_INDEX", "1"))
    parser.add_argument("--date-tag", default=os.getenv("DATE_TAG", time.strftime("%Y%m%d-%H%M%S")))
    args = parser.parse_args()

    binary = Path(args.binary).resolve()
    output_dir = Path(args.output_dir).resolve() / args.date_tag
    search_widths = parse_csv_ints(os.getenv("SEARCH_WIDTHS", "64,128"))
    requested_beam_widths = parse_csv_ints(os.getenv("BEAM_WIDTHS", "4,8,16,32"))
    prefetch_depths = parse_csv_ints(os.getenv("PREFETCH_DEPTHS", "0,1"))

    if not binary.exists():
        raise SystemExit(f"missing binary: {binary}")

    help_code, help_text = run_help(binary)
    if help_code != 0:
        raise SystemExit("agentmem_flow --help failed")
    beam_supported = "--beam-width" in help_text
    if not beam_supported:
        warning_path = output_dir / "beam_width_unsupported.json"
        write_json(
            warning_path,
            {
                **binary_metadata(root, binary),
                "status": "skipped",
                "beam_width_supported": False,
                "requested_beam_width": requested_beam_widths,
                "effective_beam_width": None,
                "warning": "beam_width was requested but not applied by current runner",
                "warnings": ["beam_width was requested but not applied by current runner"],
            },
        )

    beam_widths = requested_beam_widths if beam_supported else [None]
    failures = 0
    for search_width in search_widths:
        for beam_width in beam_widths:
            for prefetch_depth in prefetch_depths:
                tag = f"sw{search_width}"
                if beam_width is not None:
                    tag += f"_bw{beam_width}"
                tag += f"_pd{prefetch_depth}"
                output_json = output_dir / f"{tag}.json"

                if prefetch_depth not in (0, 1):
                    reason = (
                        f"prefetch_depth={prefetch_depth} skipped because "
                        "current agentmem_flow supports only prefetch_depth=1"
                    )
                    payload = skip_payload(
                        root, binary, reason, search_width, beam_width, prefetch_depth
                    )
                    payload["beam_width_supported"] = beam_supported
                    write_json(output_json, payload)
                    continue

                command = [
                    str(binary),
                    "--synthetic",
                    "0",
                    "--sift-dir",
                    args.sift_dir,
                    "--base-limit",
                    str(args.base_limit),
                    "--query-limit",
                    str(args.query_limit),
                    "--top-k",
                    str(args.top_k),
                    "--search-width",
                    str(search_width),
                    "--entry-count",
                    str(args.entry_count),
                    "--io-mode",
                    args.io_mode,
                    "--io-depth",
                    str(args.io_depth),
                    "--io-batch",
                    str(args.io_batch),
                    "--cache-policy",
                    args.cache_policy,
                    "--cache-pages",
                    str(args.cache_pages),
                    "--rebuild-index",
                    args.rebuild_index,
                    "--index-path",
                    str(output_dir / f"{tag}.idx"),
                    "--output-json",
                    str(output_json),
                ]
                if beam_width is not None:
                    command.extend(["--beam-width", str(beam_width)])
                if prefetch_depth == 0:
                    command.extend(["--enable-prefetch", "0"])
                else:
                    command.extend(["--prefetch-depth", "1"])

                completed = subprocess.run(command, cwd=root, text=True)
                if completed.returncode != 0:
                    failures += 1
                    payload = skip_payload(
                        root,
                        binary,
                        f"agentmem_flow failed with exit code {completed.returncode}",
                        search_width,
                        beam_width,
                        prefetch_depth,
                    )
                    payload["beam_width_supported"] = beam_supported
                    write_json(output_json, payload)

    if failures:
        raise SystemExit(f"{failures} agentmem_flow runs failed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
