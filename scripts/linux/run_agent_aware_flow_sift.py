#!/usr/bin/env python3
import argparse
import json
import os
import shlex
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


def write_text(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value)


def skip_payload(
    root,
    binary,
    reason,
    search_width,
    beam_width,
    prefetch_depth,
    rebuild_index_passed=False,
    extra_args=None,
    returncode=None,
):
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
        "rebuild_index_passed": rebuild_index_passed,
        "extra_args": extra_args or [],
        "warning": reason,
        "warnings": [reason],
    }
    if returncode is not None:
        payload["returncode"] = returncode
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


def write_command_file(
    path,
    root,
    command,
    search_width,
    beam_width,
    prefetch_depth,
    rebuild_index_passed,
    extra_args,
):
    lines = [
        f"cwd={shlex.quote(str(root))}",
        f"command={shlex.join([str(part) for part in command])}",
        f"search_width={search_width}",
        f"beam_width={beam_width}",
        f"prefetch_depth={prefetch_depth}",
        f"rebuild_index_passed={int(rebuild_index_passed)}",
        f"extra_args={shlex.join(extra_args) if extra_args else ''}",
    ]
    write_text(path, "\n".join(lines) + "\n")


def enrich_success_json(path, root, binary, rebuild_index_passed, extra_args):
    try:
        payload = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        payload = skip_payload(
            root,
            binary,
            f"agent_aware_flow did not write valid JSON: {error}",
            None,
            None,
            None,
            rebuild_index_passed,
            extra_args,
        )
    payload.update(binary_metadata(root, binary))
    payload["rebuild_index_passed"] = rebuild_index_passed
    payload["extra_args"] = extra_args
    write_json(path, payload)


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(
        description="Run agent_aware_flow SIFT matrix experiments."
    )
    parser.add_argument("--binary", default=str(root / "build" / "agent_aware_flow"))
    parser.add_argument("--output-dir", default=str(root / "logs" / "sift_bench"))
    parser.add_argument(
        "--date-tag", default=os.getenv("DATE_TAG", time.strftime("%Y%m%d-%H%M%S"))
    )
    args = parser.parse_args()

    binary = Path(args.binary).resolve()
    output_dir = Path(args.output_dir).resolve() / args.date_tag
    search_widths = parse_csv_ints(os.getenv("SEARCH_WIDTHS", "128,256,350,512"))
    requested_beam_widths = parse_csv_ints(os.getenv("BEAM_WIDTHS", "8,16,32"))
    prefetch_depths = parse_csv_ints(os.getenv("PREFETCH_DEPTHS", "0,1"))
    extra_args = shlex.split(os.getenv("EXTRA_ARGS", ""))
    prefetch_policy = os.getenv("PREFETCH_POLICY")
    prefetch_width = os.getenv("PREFETCH_WIDTH")
    rebuild_pending = os.getenv("REBUILD_INDEX") == "1"

    if not binary.exists():
        raise SystemExit(f"missing binary: {binary}")

    help_code, help_text = run_help(binary)
    if help_code != 0:
        raise SystemExit("agent_aware_flow --help failed")
    beam_supported = "--beam-width" in help_text

    output_dir.mkdir(parents=True, exist_ok=True)
    failures = 0
    skipped = 0
    completed_runs = 0

    for search_width in search_widths:
        for beam_width in requested_beam_widths:
            for prefetch_depth in prefetch_depths:
                tag = f"sw{search_width}_bw{beam_width}_pd{prefetch_depth}"
                output_json = output_dir / f"{tag}.json"
                command_path = output_dir / f"{tag}.command.txt"
                stdout_path = output_dir / f"{tag}.stdout.log"
                stderr_path = output_dir / f"{tag}.stderr.log"

                if not beam_supported:
                    reason = "--beam-width is not supported by this agent_aware_flow"
                    write_command_file(
                        command_path,
                        root,
                        [str(binary), "--help"],
                        search_width,
                        beam_width,
                        prefetch_depth,
                        False,
                        extra_args,
                    )
                    payload = skip_payload(
                        root, binary, reason, search_width, beam_width, prefetch_depth
                    )
                    payload["beam_width_supported"] = False
                    write_json(output_json, payload)
                    skipped += 1
                    continue

                if prefetch_depth not in (0, 1):
                    reason = (
                        f"prefetch_depth={prefetch_depth} skipped because "
                        "current agent_aware_flow supports only 0 or 1"
                    )
                    write_command_file(
                        command_path,
                        root,
                        [str(binary), "--help"],
                        search_width,
                        beam_width,
                        prefetch_depth,
                        False,
                        extra_args,
                    )
                    payload = skip_payload(
                        root, binary, reason, search_width, beam_width, prefetch_depth
                    )
                    payload["beam_width_supported"] = True
                    write_json(output_json, payload)
                    skipped += 1
                    continue

                rebuild_index_passed = rebuild_pending
                command = [
                    str(binary),
                    "--search-width",
                    str(search_width),
                    "--beam-width",
                    str(beam_width),
                    "--output-json",
                    str(output_json),
                ]

                if prefetch_depth == 0:
                    command.extend(["--enable-prefetch", "0"])
                else:
                    command.extend(["--prefetch-depth", "1"])
                    if prefetch_policy:
                        command.extend(["--prefetch-policy", prefetch_policy])
                    if prefetch_width:
                        command.extend(["--prefetch-width", prefetch_width])

                if rebuild_index_passed:
                    command.extend(["--rebuild-index", "1"])
                    rebuild_pending = False

                command.extend(extra_args)

                write_command_file(
                    command_path,
                    root,
                    command,
                    search_width,
                    beam_width,
                    prefetch_depth,
                    rebuild_index_passed,
                    extra_args,
                )

                completed = subprocess.run(
                    command,
                    cwd=root,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    check=False,
                )
                write_text(stdout_path, completed.stdout)
                write_text(stderr_path, completed.stderr)

                if completed.returncode != 0:
                    failures += 1
                    reason = f"agent_aware_flow failed with exit code {completed.returncode}"
                    payload = skip_payload(
                        root,
                        binary,
                        reason,
                        search_width,
                        beam_width,
                        prefetch_depth,
                        rebuild_index_passed,
                        extra_args,
                        completed.returncode,
                    )
                    payload["beam_width_supported"] = True
                    write_json(output_json, payload)
                    print(f"{tag}: failed")
                    continue

                enrich_success_json(
                    output_json, root, binary, rebuild_index_passed, extra_args
                )
                completed_runs += 1
                print(f"{tag}: completed")

    print(
        f"matrix output: {output_dir} "
        f"(completed={completed_runs}, skipped={skipped}, failed={failures})"
    )
    if failures:
        raise SystemExit(f"{failures} agent_aware_flow runs failed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
