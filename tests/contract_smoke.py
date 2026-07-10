#!/usr/bin/env python3
import csv
import importlib.util
import json
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


def main():
    search, mixed, tools = map(Path, sys.argv[1:4])
    with tempfile.TemporaryDirectory(prefix="agent-aware-smoke-") as directory:
        root = Path(directory)
        index, result = root / "smoke.idx", root / "search.json"
        common = [
            str(search), "--synthetic", "1", "--base-limit", "512",
            "--query-limit", "8", "--synthetic-dim", "32",
            "--synthetic-clusters", "8", "--pq-subspaces", "8",
            "--pq-centroids", "16", "--pq-train-limit", "256",
            "--pq-iterations", "2", "--graph-degree", "16",
            "--build-policy", "vamana", "--packing-strategy", "bfs",
            "--index-path", str(index), "--io-mode", "pread",
            "--enable-prefetch", "0", "--cache-pages", "32",
        ]
        run([*common, "--rebuild-index", "1", "--output-json", str(result)])
        payload = json.loads(result.read_text(encoding="utf-8"))
        assert payload["status"] == "completed"
        assert payload["build_policy"] == "vamana"
        assert payload["packing_strategy"] == "bfs"
        assert {"expanded", "submitted_reads", "cache_hits"} <= payload["stats"].keys()

        alias_result = root / "alias.json"
        run([*common, "--rebuild-index", "0", "--output-json", str(alias_result),
             "--k", "5", "--beam_width", "4"])
        alias_payload = json.loads(alias_result.read_text(encoding="utf-8"))
        assert alias_payload["top_k"] == 5
        assert alias_payload["effective_beam_width"] == 4
        rejected = run([str(search), "--build-policy", "exact"], ok=False)
        assert "only vamana" in rejected.stderr
        rejected = run([str(search), "--packing-strategy", "coaccess"], ok=False)
        assert "only bfs" in rejected.stderr

        fallback_result = root / "fallback.json"
        run([*common, "--index-path", str(root / "fallback.idx"),
             "--page-size", "5000", "--io-mode", "io_uring",
             "--rebuild-index", "1", "--output-json", str(fallback_result)])
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

        help_text = run([sys.executable, str(tools), "--help"]).stdout
        for command in ("once", "ab", "tune", "analyze", "validate",
                        "train", "replay", "summarize"):
            assert command in help_text
            run([sys.executable, str(tools), command, "--help"])

        run([sys.executable, str(tools), "once", "--dry-run", "--no-build",
             "--output-dir", str(root / "once")])
        ab_run = run([sys.executable, str(tools), "ab", "--dry-run", "--no-build",
                      "--cache-conditions", "hot", "--seeds", "42,43,44",
                      "--output-root", str(root / "ab")]).stdout
        expected_order = (
            "no_prefetch", "stage1_prebeam", "stage2_composite",
            "stage1_prebeam", "stage2_composite", "no_prefetch",
            "stage2_composite", "no_prefetch", "stage1_prebeam",
        )
        offsets = []
        for arm in expected_order:
            offsets.append(ab_run.index(
                f"--policy-version {arm}", offsets[-1] + 1 if offsets else 0))
        assert offsets == sorted(offsets)
        run([sys.executable, str(tools), "tune", "--dry-run", "--no-build",
             "--output-root", str(root / "tune")])

        trace = root / "trace.csv"
        fields = [
            "run_id", "policy_version", "seed", "query_id", "step_id",
            "first_seen_step", "estimated_demand_step", "trigger", "group_id",
            "decision_id", "submission_id", "candidate_page_id",
            "selected_by_policy", "was_prefetched", "was_demanded", "hit_type",
            "saved_wait_us", "was_ready_before_demand", "is_cached_at_decision",
            "is_inflight_at_decision", "is_ready_at_decision",
            "is_materialized_at_decision", "num_candidates_on_page",
            "min_pq_rank_on_page", "avg_pq_rank_on_page", "candidate_rank_span",
            "contains_top1_candidate", "contains_topk_candidate", "io_queue_depth",
            "cache_pressure", "label",
        ]
        with trace.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader()
            for query_id in range(3):
                base = dict.fromkeys(fields, "0")
                base.update(run_id="test", policy_version="test", seed="42",
                            query_id=str(query_id), step_id="1", first_seen_step="1",
                            estimated_demand_step="2", trigger="post_expand",
                            group_id=f"g{query_id}", num_candidates_on_page="2",
                            min_pq_rank_on_page="1", avg_pq_rank_on_page="1.5",
                            candidate_rank_span="1", contains_top1_candidate="1",
                            contains_topk_candidate="1")
                useful = dict(base, decision_id=str(query_id * 2 + 1),
                              submission_id=str(query_id + 1),
                              candidate_page_id=str(query_id * 2),
                              selected_by_policy="1", was_prefetched="1",
                              was_demanded="1", hit_type="ready_hit",
                              saved_wait_us="10", was_ready_before_demand="1", label="1")
                unused = dict(base, decision_id=str(query_id * 2 + 2),
                              submission_id="", candidate_page_id=str(query_id * 2 + 1),
                              hit_type="", label="0")
                writer.writerows((useful, unused))

        run([sys.executable, str(tools), "analyze", "--trace", str(trace),
             "--json-out", str(root / "trace-summary.json")])
        run([sys.executable, str(tools), "validate", "--trace", str(trace),
             "--json-out", str(root / "trace-validation.json")])
        model = root / "model.txt"
        model.write_text("base_score=0\nbooster[0]:\n0:leaf=0\n", encoding="utf-8")
        run([sys.executable, str(tools), "replay", "--trace", str(trace),
             "--model", str(model), "--top-k", "1",
             "--out-json", str(root / "replay.json")])
        if importlib.util.find_spec("xgboost"):
            run([sys.executable, str(tools), "train", "--trace", str(trace),
                 "--model-out", str(root / "trained.txt"), "--train-all", "--rounds", "1"])


if __name__ == "__main__":
    main()
