#!/usr/bin/env python3
import argparse
import json
import os
import shlex
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path


PARAMETER_RATIONALE = {
    "search_width": (
        "Directly sets DiskGraphSearchConfig.search_width and bounds graph "
        "expansions, so it should move Recall, visited/page reads, QPS, and "
        "tail latency."
    ),
    "beam_width": (
        "Directly sets DiskGraphSearchConfig.beam_width and controls per-round "
        "candidate expansion/batch size, so it should move I/O batching, "
        "submit counts, QPS, and P95/P99."
    ),
    "memory_budget_ratio": (
        "Feeds the automatic cache_pages calculation when cache_pages is not "
        "explicitly set, so it should move cache_bytes/cache_hit_rate and "
        "latency under the memory budget."
    ),
    "io_mode": (
        "Flows into PackedDiskGraphIndex.configure_io and AsyncPageReader, so "
        "it should change sync/odirect/io_uring behavior and I/O wait stats."
    ),
    "prefetch_depth": (
        "0 disables prefetch; positive values scale the query-level prefetch "
        "budget with prefetch_width when effective io_uring is available. "
        "The staged io_mode sweep keeps it disabled by default so I/O mode is "
        "not conflated with prefetch."
    ),
}


@dataclass(frozen=True)
class RunSpec:
    phase: str
    axis: str
    search_width: int
    beam_width: int
    memory_budget_ratio: float
    io_mode: str
    prefetch_depth: int
    repeat: int = 1

    @property
    def tag(self):
        ratio = f"{self.memory_budget_ratio:.2f}".replace(".", "")
        return (
            f"{self.phase}_sw{self.search_width}_bw{self.beam_width}_"
            f"m{ratio}_{self.io_mode}_pd{self.prefetch_depth}_r{self.repeat}"
        )


def parse_csv_ints(value):
    output = []
    for item in value.split(","):
        item = item.strip()
        if item:
            output.append(int(item))
    return output


def parse_csv_floats(value):
    output = []
    for item in value.split(","):
        item = item.strip()
        if item:
            output.append(float(item))
    return output


def parse_csv_strings(value):
    output = []
    for item in value.split(","):
        item = item.strip()
        if item:
            output.append(item)
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


def skip_payload(root, binary, spec, reason, extra_args, returncode=None):
    payload = {
        "status": "skipped",
        "matrix_phase": spec.phase,
        "matrix_axis": spec.axis,
        "matrix_parameter_rationale": PARAMETER_RATIONALE.get(spec.axis, ""),
        "requested_search_width": spec.search_width,
        "effective_search_width": None,
        "requested_beam_width": spec.beam_width,
        "effective_beam_width": None,
        "beam_width_supported": None,
        "requested_prefetch_depth": spec.prefetch_depth,
        "effective_prefetch_depth": None,
        "memory_budget_ratio": spec.memory_budget_ratio,
        "io_requested_mode": spec.io_mode,
        "prefetch_enabled": False,
        "io_uring_enabled": False,
        "odirect_enabled": False,
        "rebuild_index_passed": False,
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


def write_command_file(path, root, command, spec, rebuild_index_passed, extra_args):
    lines = [
        f"cwd={shlex.quote(str(root))}",
        f"command={shlex.join([str(part) for part in command])}",
        f"matrix_phase={spec.phase}",
        f"matrix_axis={spec.axis}",
        f"search_width={spec.search_width}",
        f"beam_width={spec.beam_width}",
        f"memory_budget_ratio={spec.memory_budget_ratio:.6g}",
        f"io_mode={spec.io_mode}",
        f"prefetch_depth={spec.prefetch_depth}",
        f"rebuild_index_passed={int(rebuild_index_passed)}",
        f"extra_args={shlex.join(extra_args) if extra_args else ''}",
    ]
    write_text(path, "\n".join(lines) + "\n")


def enrich_success_json(path, root, binary, spec, rebuild_index_passed, extra_args):
    try:
        payload = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        payload = skip_payload(
            root,
            binary,
            spec,
            f"agent_aware_flow did not write valid JSON: {error}",
            extra_args,
        )
    payload.update(binary_metadata(root, binary))
    payload["matrix_phase"] = spec.phase
    payload["matrix_axis"] = spec.axis
    payload["matrix_parameter_rationale"] = PARAMETER_RATIONALE.get(spec.axis, "")
    payload["matrix_requested"] = {
        "search_width": spec.search_width,
        "beam_width": spec.beam_width,
        "memory_budget_ratio": spec.memory_budget_ratio,
        "io_mode": spec.io_mode,
        "prefetch_depth": spec.prefetch_depth,
        "repeat": spec.repeat,
    }
    payload["rebuild_index_passed"] = rebuild_index_passed
    payload["extra_args"] = extra_args
    write_json(path, payload)


def has_explicit_option(extra_args, names):
    for arg in extra_args:
        for name in names:
            if arg == name or arg.startswith(name + "="):
                return True
    return False


def build_staged_specs(args):
    specs = []
    repeat = 1
    for sw in args.search_widths:
        specs.append(
            RunSpec(
                "m1_search_width",
                "search_width",
                sw,
                args.final_beam_width,
                args.final_memory_budget_ratio,
                args.final_io_mode,
                args.prefetch_depth,
                repeat,
            )
        )

    for sw in args.beam_search_widths:
        for bw in args.beam_widths:
            specs.append(
                RunSpec(
                    "m2_beam_width",
                    "beam_width",
                    sw,
                    bw,
                    args.final_memory_budget_ratio,
                    args.final_io_mode,
                    args.prefetch_depth,
                    repeat,
                )
            )

    for ratio in args.memory_budget_ratios:
        specs.append(
            RunSpec(
                "m3_memory_budget",
                "memory_budget_ratio",
                args.final_search_width,
                args.final_beam_width,
                ratio,
                args.final_io_mode,
                args.prefetch_depth,
                repeat,
            )
        )

    for io_mode in args.io_modes:
        specs.append(
            RunSpec(
                "m4_io_mode",
                "io_mode",
                args.final_search_width,
                args.final_beam_width,
                args.final_memory_budget_ratio,
                io_mode,
                args.io_mode_prefetch_depth,
                repeat,
            )
        )

    if args.confirm_repeats > 0:
        for repeat in range(1, args.confirm_repeats + 1):
            for sw in args.confirm_search_widths:
                specs.append(
                    RunSpec(
                        "m5_confirm",
                        "confirm_repeat",
                        sw,
                        args.final_beam_width,
                        args.final_memory_budget_ratio,
                        args.final_io_mode,
                        args.prefetch_depth,
                        repeat,
                    )
                )

    return specs


def build_grid_specs(args):
    specs = []
    repeat = 1
    for sw in args.search_widths:
        for bw in args.beam_widths:
            for ratio in args.memory_budget_ratios:
                for io_mode in args.io_modes:
                    specs.append(
                        RunSpec(
                            "grid",
                            "grid",
                            sw,
                            bw,
                            ratio,
                            io_mode,
                            args.prefetch_depth,
                            repeat,
                        )
                    )
    return specs


def build_legacy_specs(args):
    specs = []
    for sw in args.search_widths:
        for bw in args.beam_widths:
            for pd in args.prefetch_depths:
                specs.append(
                    RunSpec(
                        "legacy_prefetch",
                        "prefetch_depth",
                        sw,
                        bw,
                        args.final_memory_budget_ratio,
                        args.final_io_mode,
                        pd,
                    )
                )
    return specs


def build_specs(args):
    if args.matrix_plan == "staged":
        return build_staged_specs(args)
    if args.matrix_plan == "grid":
        return build_grid_specs(args)
    if args.matrix_plan == "legacy":
        return build_legacy_specs(args)
    raise ValueError(f"unknown matrix plan: {args.matrix_plan}")


def build_command(binary, output_json, spec, rebuild_index_passed, extra_args, args):
    command = [
        str(binary),
        "--search-width",
        str(spec.search_width),
        "--beam-width",
        str(spec.beam_width),
        "--memory-budget-ratio",
        f"{spec.memory_budget_ratio:.6g}",
        "--io-mode",
        spec.io_mode,
        "--cache-policy",
        args.cache_policy,
        "--output-json",
        str(output_json),
    ]

    if spec.prefetch_depth == 0:
        command.extend(["--enable-prefetch", "0"])
    else:
        command.extend(["--prefetch-depth", str(spec.prefetch_depth)])
        if args.prefetch_policy:
            command.extend(["--prefetch-policy", args.prefetch_policy])
        if args.prefetch_width:
            command.extend(["--prefetch-width", str(args.prefetch_width)])

    if rebuild_index_passed:
        command.extend(["--rebuild-index", "1"])

    command.extend(extra_args)
    return command


def write_plan_metadata(path, args, specs, warnings):
    payload = {
        "status": "planned",
        "matrix_plan": args.matrix_plan,
        "run_count": len(specs),
        "parameter_rationale": PARAMETER_RATIONALE,
        "defaults": {
            "search_widths": args.search_widths,
            "beam_widths": args.beam_widths,
            "beam_search_widths": args.beam_search_widths,
            "memory_budget_ratios": args.memory_budget_ratios,
            "io_modes": args.io_modes,
            "cache_policy": args.cache_policy,
            "prefetch_depth": args.prefetch_depth,
            "io_mode_prefetch_depth": args.io_mode_prefetch_depth,
            "prefetch_policy": args.prefetch_policy,
            "prefetch_width": args.prefetch_width,
            "final_search_width": args.final_search_width,
            "final_beam_width": args.final_beam_width,
            "final_memory_budget_ratio": args.final_memory_budget_ratio,
            "final_io_mode": args.final_io_mode,
            "confirm_repeats": args.confirm_repeats,
        },
        "warnings": warnings,
        "runs": [
            {
                "tag": spec.tag,
                "phase": spec.phase,
                "axis": spec.axis,
                "search_width": spec.search_width,
                "beam_width": spec.beam_width,
                "memory_budget_ratio": spec.memory_budget_ratio,
                "io_mode": spec.io_mode,
                "prefetch_depth": spec.prefetch_depth,
                "repeat": spec.repeat,
            }
            for spec in specs
        ],
    }
    write_json(path, payload)


def visualize(root, matrix_output_dir, visualize_output_dir):
    script = root / "scripts" / "plot_sift_matrix.py"
    if not script.exists():
        print(f"visualization skipped: missing {script}", file=sys.stderr)
        return 1
    command = [
        sys.executable,
        str(script),
        str(matrix_output_dir),
        "--output-dir",
        str(visualize_output_dir),
    ]
    completed = subprocess.run(command, cwd=root, check=False)
    return completed.returncode


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(
        description="Run staged agent_aware_flow SIFT parameter matrix experiments."
    )
    parser.add_argument("--binary", default=str(root / "build" / "agent_aware_flow"))
    parser.add_argument("--output-dir", default=str(root / "logs" / "sift_bench"))
    parser.add_argument(
        "--date-tag", default=os.getenv("DATE_TAG", time.strftime("%Y%m%d-%H%M%S"))
    )
    parser.add_argument(
        "--matrix-plan",
        choices=("staged", "grid", "legacy"),
        default=os.getenv("MATRIX_PLAN", "staged"),
    )
    parser.add_argument(
        "--visualize",
        action=argparse.BooleanOptionalAction,
        default=os.getenv("VISUALIZE", "1") != "0",
    )
    parser.add_argument("--visualize-output-dir", default=None)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--skip-existing", action="store_true")
    parser.add_argument("--fail-fast", action="store_true")
    parser.add_argument(
        "--search-widths",
        type=parse_csv_ints,
        default=parse_csv_ints(os.getenv("SEARCH_WIDTHS", "128,192,256,350,512")),
    )
    parser.add_argument(
        "--beam-widths",
        type=parse_csv_ints,
        default=parse_csv_ints(os.getenv("BEAM_WIDTHS", "8,16,32")),
    )
    parser.add_argument(
        "--beam-search-widths",
        type=parse_csv_ints,
        default=parse_csv_ints(os.getenv("BEAM_SEARCH_WIDTHS", "256,350")),
    )
    parser.add_argument(
        "--memory-budget-ratios",
        type=parse_csv_floats,
        default=parse_csv_floats(os.getenv("MEMORY_BUDGET_RATIOS", "0.10,0.15,0.20")),
    )
    parser.add_argument(
        "--io-modes",
        type=parse_csv_strings,
        default=parse_csv_strings(os.getenv("IO_MODES", "pread,odirect,io_uring")),
    )
    parser.add_argument(
        "--prefetch-depths",
        type=parse_csv_ints,
        default=parse_csv_ints(os.getenv("PREFETCH_DEPTHS", "0,1")),
        help="Only used by --matrix-plan legacy.",
    )
    parser.add_argument(
        "--prefetch-depth",
        type=int,
        default=int(os.getenv("PREFETCH_DEPTH", "1")),
        help="Default staged/grid prefetch depth; 0 disables prefetch.",
    )
    parser.add_argument(
        "--io-mode-prefetch-depth",
        type=int,
        default=int(os.getenv("IO_MODE_PREFETCH_DEPTH", "0")),
        help="Prefetch depth for staged m4 io_mode sweep; default 0 isolates I/O mode.",
    )
    parser.add_argument("--prefetch-policy", default=os.getenv("PREFETCH_POLICY", "frontier-next-hop"))
    parser.add_argument("--prefetch-width", type=int, default=int(os.getenv("PREFETCH_WIDTH", "4")))
    parser.add_argument("--cache-policy", default=os.getenv("CACHE_POLICY", "graph-aware-2q"))
    parser.add_argument("--final-search-width", type=int, default=int(os.getenv("FINAL_SEARCH_WIDTH", "256")))
    parser.add_argument("--final-beam-width", type=int, default=int(os.getenv("FINAL_BEAM_WIDTH", "16")))
    parser.add_argument(
        "--final-memory-budget-ratio",
        type=float,
        default=float(os.getenv("FINAL_MEMORY_BUDGET_RATIO", "0.20")),
    )
    parser.add_argument("--final-io-mode", default=os.getenv("FINAL_IO_MODE", "io_uring"))
    parser.add_argument("--confirm-repeats", type=int, default=int(os.getenv("CONFIRM_REPEATS", "0")))
    parser.add_argument(
        "--confirm-search-widths",
        type=parse_csv_ints,
        default=parse_csv_ints(os.getenv("CONFIRM_SEARCH_WIDTHS", "256,350")),
    )
    args = parser.parse_args()

    binary = Path(args.binary).resolve()
    output_dir = Path(args.output_dir).resolve() / args.date_tag
    visualize_output_dir = (
        Path(args.visualize_output_dir).resolve()
        if args.visualize_output_dir
        else output_dir / "visualization"
    )
    extra_args = shlex.split(os.getenv("EXTRA_ARGS", ""))
    rebuild_pending = os.getenv("REBUILD_INDEX") == "1"

    if not binary.exists():
        raise SystemExit(f"missing binary: {binary}")

    help_code, help_text = run_help(binary)
    if help_code != 0:
        raise SystemExit("agent_aware_flow --help failed")
    beam_supported = "--beam-width" in help_text

    warnings = []
    if has_explicit_option(extra_args, ["--cache-pages", "--cache_pages"]):
        warnings.append(
            "memory_budget_ratio will not change cache_pages because EXTRA_ARGS "
            "contains an explicit cache-pages override"
        )
    specs = build_specs(args)
    output_dir.mkdir(parents=True, exist_ok=True)
    write_plan_metadata(output_dir / "matrix_plan.json", args, specs, warnings)

    if args.dry_run:
        for spec in specs:
            command = build_command(binary, output_dir / f"{spec.tag}.json", spec, False, extra_args, args)
            print(shlex.join([str(part) for part in command]))
        print(f"dry-run matrix output: {output_dir} (planned={len(specs)})")
        return 0

    failures = 0
    skipped = 0
    completed_runs = 0

    for spec in specs:
        output_json = output_dir / f"{spec.tag}.json"
        command_path = output_dir / f"{spec.tag}.command.txt"
        stdout_path = output_dir / f"{spec.tag}.stdout.log"
        stderr_path = output_dir / f"{spec.tag}.stderr.log"

        if args.skip_existing and output_json.exists():
            print(f"{spec.tag}: existing")
            skipped += 1
            continue

        if not beam_supported:
            reason = "--beam-width is not supported by this agent_aware_flow"
            write_command_file(command_path, root, [str(binary), "--help"], spec, False, extra_args)
            payload = skip_payload(root, binary, spec, reason, extra_args)
            payload["beam_width_supported"] = False
            write_json(output_json, payload)
            skipped += 1
            continue

        rebuild_index_passed = rebuild_pending
        command = build_command(binary, output_json, spec, rebuild_index_passed, extra_args, args)
        if rebuild_index_passed:
            rebuild_pending = False

        write_command_file(command_path, root, command, spec, rebuild_index_passed, extra_args)

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
                spec,
                reason,
                extra_args,
                completed.returncode,
            )
            payload["beam_width_supported"] = True
            write_json(output_json, payload)
            print(f"{spec.tag}: failed")
            if args.fail_fast:
                break
            continue

        enrich_success_json(output_json, root, binary, spec, rebuild_index_passed, extra_args)
        completed_runs += 1
        print(f"{spec.tag}: completed")

    print(
        f"matrix output: {output_dir} "
        f"(completed={completed_runs}, skipped={skipped}, failed={failures})"
    )

    viz_code = 0
    if args.visualize:
        viz_code = visualize(root, output_dir, visualize_output_dir)
        if viz_code == 0:
            print(f"matrix visualization: {visualize_output_dir}")
        else:
            print(f"matrix visualization failed with exit code {viz_code}", file=sys.stderr)

    if failures:
        raise SystemExit(f"{failures} agent_aware_flow runs failed")
    if viz_code:
        raise SystemExit(viz_code)
    return 0


if __name__ == "__main__":
    sys.exit(main())
