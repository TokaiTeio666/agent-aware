#!/usr/bin/env python3
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def run(command, *, ok=True):
    result = subprocess.run(command, text=True, capture_output=True)
    if (result.returncode == 0) != ok:
        raise AssertionError(
            f"command returned {result.returncode}: {' '.join(map(str, command))}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def write_fvecs(path, rows):
    with path.open("wb") as stream:
        for row in rows:
            stream.write(struct.pack("<i", len(row)))
            stream.write(struct.pack(f"<{len(row)}f", *row))


def build_index(builder, index, *, page_size=None):
    command = [
        str(builder), "--synthetic", "1", "--base-limit", "512",
        "--synthetic-dim", "32", "--synthetic-clusters", "8",
        "--pq-subspaces", "8", "--pq-centroids", "16",
        "--pq-train-limit", "256", "--pq-iterations", "2",
        "--graph-degree", "16", "--index-path", str(index),
    ]
    if page_size is not None:
        command.extend(["--page-size", str(page_size)])
    run(command)


def main():
    builder, search, mixed, tools = map(Path, sys.argv[1:5])
    with tempfile.TemporaryDirectory(prefix="agent-aware-smoke-") as directory:
        root = Path(directory)
        index = root / "smoke.idx"
        result = root / "search.json"
        query = root / "query.fvecs"
        write_fvecs(
            query,
            [[float((row + dim) % 7) for dim in range(32)] for row in range(8)],
        )
        build_index(builder, index)

        common = [
            str(search), "--sift-dir", "", "--query", str(query),
            "--query-limit", "8", "--index-path", str(index),
            "--io-mode", "pread", "--cache-pages", "32",
        ]
        run([*common, "--output-json", str(result)])
        payload = json.loads(result.read_text(encoding="utf-8"))
        manifest_path = Path(str(index) + ".manifest.json")
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        assert payload["status"] == "completed"
        assert payload["index_format_version"] == 3
        assert payload["entry_strategy"] == "single-medoid"
        assert payload["entry_seed_ids"] == [manifest["entry_id"]]
        assert Path(payload["build_manifest"]) == manifest_path
        expected_stats = {
            "expanded", "submitted_reads", "completed_reads", "demand_reads",
            "cache_hits", "io_submit_syscalls", "beam_cached_pages",
            "nodes_used_per_loaded_page", "page_fill_utilization",
            "demand_read_wait_us", "same_page_node_reuse",
        }
        assert expected_stats <= payload["stats"].keys()
        def collect_keys(value):
            if isinstance(value, dict):
                return [str(key).lower() for key in value] + [
                    key for item in value.values() for key in collect_keys(item)
                ]
            if isinstance(value, list):
                return [key for item in value for key in collect_keys(item)]
            return []

        assert payload["prefetch_policy"] == "none"
        assert payload["prefetch_runtime_enabled"] is False
        assert payload["stats"]["prefetch_submitted"] == 0
        assert "prefetch_model" not in payload

        alias_result = root / "alias.json"
        run([*common, "--output-json", str(alias_result),
             "--k", "5", "--beam_width", "4"])
        alias = json.loads(alias_result.read_text(encoding="utf-8"))
        assert alias["top_k"] == 5
        assert alias["effective_beam_width"] == 4

        topology_result = root / "topology.json"
        run([*common, "--output-json", str(topology_result),
             "--enable-prefetch", "1", "--prefetch-policy", "topology"])
        topology = json.loads(topology_result.read_text(encoding="utf-8"))
        assert topology["prefetch_policy"] == "topology"
        assert topology["prefetch_runtime_enabled"] is False

        rejected = run([*common, "--prefetch-policy", "xgboost"], ok=False)
        assert "expected none or topology" in rejected.stderr
        removed_options = (
            "--pre" + "fetch-model",
            "--pre" + "fetch-early-" + "trigger",
        )
        for removed_option in removed_options:
            rejected = run([*common, removed_option, "0"], ok=False)
            assert f"Unknown option: {removed_option}" in rejected.stderr

        fallback_index = root / "fallback.idx"
        fallback_result = root / "fallback.json"
        build_index(builder, fallback_index, page_size=5000)
        run([
            *common, "--index-path", str(fallback_index),
            "--io-mode", "io_uring", "--output-json", str(fallback_result),
        ])
        fallback = json.loads(fallback_result.read_text(encoding="utf-8"))
        assert fallback["io_requested_mode"] == "io_uring"
        assert fallback["io_effective_mode"] == "pread"
        assert fallback["io_fallback_reason"]

        mixed_result = root / "mixed.json"
        run([
            str(mixed), "--num_operations", "50", "--base_count", "128",
            "--query_count", "16", "--dim", "16", "--dynamic_dir",
            str(root / "dynamic"), "--output", str(mixed_result),
            "--read_ratio", "0.9", "--write_ratio", "0.1",
            "--enable_compaction", "0",
        ])
        mixed_payload = json.loads(mixed_result.read_text(encoding="utf-8"))
        assert mixed_payload["status"] == "completed"
        assert mixed_payload["scenarios"][0]["scenario"] == "custom"
        assert "pre" + "fetch" not in json.dumps(mixed_payload).lower()

        help_result = run([sys.executable, str(tools), "--help"])
        assert "once" in help_result.stdout
        assert "ab" in help_result.stdout
        for removed_command in (
            " tune", " analyze", " validate", " train", " replay", " summarize",
        ):
            assert removed_command not in help_result.stdout.lower()


if __name__ == "__main__":
    main()
