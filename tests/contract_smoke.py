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
            # 本测试校验的是输出契约，不是内存预算：512 条 × 32 维的合成数据集
            # 只有 64 KiB，20% 预算远小于进程自身的固定占用，开启硬失败会以
            # "预算不可满足"退出。预算是独立指标，由其专项实验负责。
            "--memory-budget-enforce", "0",
        ]
        run([*common, "--output-json", str(result)])
        payload = json.loads(result.read_text(encoding="utf-8"))
        manifest_path = Path(str(index) + ".manifest.json")
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        assert payload["status"] == "completed"
        assert payload["index_format_version"] == 3
        # 入口不是几何 medoid，而是构图时实测可达性最好的节点
        # （见 disk_graph_builder.cpp 的 select_entry_node）。
        assert payload["entry_strategy"] == "reachability-maximizing"
        # 搜索还会在入口之外均匀撒起点，撒点数必须如实报告。
        assert payload["spread_start_count"] > 0
        assert payload["entry_seed_ids"] == [manifest["entry_id"]]
        assert Path(payload["build_manifest"]) == manifest_path
        expected_stats = {
            "expanded", "submitted_reads", "completed_reads", "demand_reads",
            "cache_hits", "io_submit_syscalls",
            "nodes_used_per_loaded_page", "page_fill_utilization",
            "demand_read_wait_us", "demand_io_wait_us", "demand_post_cpu_us",
            "demand_read_batches", "pages_per_read_batch",
            "candidate_pushes", "candidate_pops",
            "same_page_node_reuse",
        }
        assert expected_stats <= payload["stats"].keys()
        # 恒 0 的死字段（唯一的写入者 select_candidate_batch 从未被调用）已删除，
        # 留着会让读者以为"同页复用为 0"是实测结果。JSON 结构版本随之升到 2。
        assert payload["stats_schema_version"] == 2
        assert "beam_unique_pages" not in payload["stats"]
        assert "beam_cached_pages" not in payload["stats"]
        stages = payload["stats"]["demand_read_by_stage"]
        assert set(stages) == {
            "entry_anchor", "candidate_expand", "neighbor_rank", "rerank"}
        for stage in stages.values():
            assert set(stage) == {
                "pages", "batches", "syscalls", "wait_calls", "io_wait_us",
                "post_cpu_us"}
        # 各阶段页数之和必须等于本查询实际提交的 demand 读页数：这条恒等式
        # 一旦不成立，说明有 demand 读没被归因。
        stage_pages = sum(stage["pages"] for stage in stages.values())
        assert stage_pages == payload["stats"]["page_reads"]
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
