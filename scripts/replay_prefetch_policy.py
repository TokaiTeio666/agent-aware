#!/usr/bin/env python3
"""Offline replay for XGBoost and oracle prefetch policies."""

from __future__ import annotations

import argparse
import csv
import json
import math
from collections import defaultdict
from pathlib import Path
from typing import Callable, Dict, Iterable, List, Sequence

from train_prefetch_ranker import TRACE_FEATURES, parse_float


def parse_bool(value: object) -> bool:
    if value is None:
        return False
    return str(value).strip().lower() in {"1", "true", "yes", "on"}


def parse_int(value: object) -> int:
    parsed = parse_float(value)
    return int(parsed) if parsed is not None else 0


def load_trace(path: Path) -> List[dict]:
    with path.open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    if not rows:
        raise RuntimeError(f"empty prefetch trace: {path}")
    return rows


def group_rows(rows: Iterable[dict]) -> Dict[str, List[dict]]:
    groups: Dict[str, List[dict]] = defaultdict(list)
    for row in rows:
        group_id = row.get("group_id")
        if not group_id:
            group_id = f"{row.get('query_id', 'unknown')}_{row.get('step_id', 0)}"
        groups[group_id].append(row)
    return groups


class TreeNode:
    def __init__(self) -> None:
        self.leaf = False
        self.feature = ""
        self.threshold = 0.0
        self.yes = -1
        self.no = -1
        self.missing = -1
        self.leaf_value = 0.0


def parse_int_after(text: str, key: str) -> int:
    if key not in text:
        raise RuntimeError(f"malformed XGBoost node, missing {key}")
    value = text.split(key, 1)[1].split(",", 1)[0].split("]", 1)[0]
    return int(value)


def load_xgboost_dump(path: Path) -> tuple[float, List[Dict[int, TreeNode]]]:
    base_score = 0.0
    trees: List[Dict[int, TreeNode]] = []
    current: Dict[int, TreeNode] | None = None
    with path.open(encoding="utf-8") as handle:
        for raw in handle:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            if line.startswith("base_score="):
                base_score = float(line.split("=", 1)[1])
                continue
            if line.startswith("booster[") or line.startswith("tree["):
                current = {}
                trees.append(current)
                continue
            if ":" not in line:
                continue
            if current is None:
                current = {}
                trees.append(current)
            node_id_text, body = line.split(":", 1)
            node = TreeNode()
            if body.startswith("leaf="):
                node.leaf = True
                node.leaf_value = float(body.split("leaf=", 1)[1].split(",", 1)[0])
            else:
                split = body.split("[", 1)[1].split("]", 1)[0]
                feature, threshold = split.split("<", 1)
                node.feature = feature
                node.threshold = float(threshold)
                node.yes = parse_int_after(body, "yes=")
                node.no = parse_int_after(body, "no=")
                node.missing = parse_int_after(body, "missing=")
            current[int(node_id_text)] = node
    if not trees:
        raise RuntimeError(f"empty XGBoost model: {path}")
    return base_score, trees


def row_label(row: dict) -> int:
    for name in ("label", "prefetch_label"):
        if name in row:
            return parse_int(row.get(name))
    return 1 if parse_bool(row.get("was_demanded")) else 0


def is_positive(row: dict) -> bool:
    return row_label(row) > 0 or parse_bool(row.get("was_demanded"))


def current_score(row: dict) -> float:
    count = parse_float(row.get("num_candidates_on_page")) or 0.0
    rank = parse_float(row.get("min_pq_rank_on_page")) or 0.0
    return count * 1_000_000.0 - rank


def trace_score(row: dict) -> float:
    score = parse_float(row.get("prefetch_score"))
    return score if score is not None else current_score(row)


def feature_value(row: dict, feature_name: str) -> float:
    for trace_name, model_name in TRACE_FEATURES:
        if model_name == feature_name:
            return parse_float(row.get(trace_name)) or 0.0
    return 0.0


def xgboost_score(row: dict, base_score: float, trees: List[Dict[int, TreeNode]]) -> float:
    score = base_score
    for tree in trees:
        node_id = 0
        while True:
            node = tree[node_id]
            if node.leaf:
                score += node.leaf_value
                break
            value = feature_value(row, node.feature)
            if not math.isfinite(value):
                node_id = node.missing
            else:
                node_id = node.yes if value < node.threshold else node.no
    return score


def oracle_score(row: dict) -> float:
    label = row_label(row)
    demanded = 1 if is_positive(row) else 0
    rank = parse_float(row.get("min_pq_rank_on_page")) or 0.0
    return demanded * 10_000.0 + label * 100.0 - rank


def replay(
    groups: Dict[str, List[dict]],
    scorer: Callable[[dict], float],
    top_k: int,
) -> dict:
    selected_pages = 0
    positive_pages = 0
    selected_positive_pages = 0

    for group in groups.values():
        positives = [row for row in group if is_positive(row)]
        positive_pages += len(positives)
        ranked = sorted(group, key=scorer, reverse=True)
        selected = ranked[:top_k]
        selected_pages += len(selected)
        selected_positive_pages += sum(1 for row in selected if is_positive(row))

    return {
        "groups": len(groups),
        "selected_pages": selected_pages,
        "positive_pages": positive_pages,
        "selected_positive_pages": selected_positive_pages,
        "precision_at_k": selected_positive_pages / selected_pages
        if selected_pages
        else None,
        "recall_at_k": selected_positive_pages / positive_pages
        if positive_pages
        else None,
    }


def make_markdown(payload: dict) -> str:
    lines = [
        "# Prefetch XGBoost Replay",
        "",
        "| Policy | Top-K | Groups | Selected | Positives | Selected Positives | Precision@K | Recall@K | Oracle Gap |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for row in payload["results"]:
        def fmt(value):
            if value is None:
                return "null"
            if isinstance(value, float):
                return f"{value:.6g}"
            return str(value)

        lines.append(
            "| {policy} | {top_k} | {groups} | {selected_pages} | {positive_pages} | "
            "{selected_positive_pages} | {precision_at_k} | {recall_at_k} | {oracle_gap} |".format(
                policy=row["policy"],
                top_k=row["top_k"],
                groups=row["groups"],
                selected_pages=row["selected_pages"],
                positive_pages=row["positive_pages"],
                selected_positive_pages=row["selected_positive_pages"],
                precision_at_k=fmt(row["precision_at_k"]),
                recall_at_k=fmt(row["recall_at_k"]),
                oracle_gap=fmt(row["oracle_gap"]),
            )
        )

    lines.extend(["", "## Diagnostics", ""])
    max_oracle = max(
        (row["recall_at_k"] or 0.0 for row in payload["results"] if row["policy"] == "oracle"),
        default=0.0,
    )
    max_xgboost = max(
        (row["recall_at_k"] or 0.0 for row in payload["results"] if row["policy"] == "xgboost"),
        default=0.0,
    )
    diagnostics_added = False
    if max_oracle < 0.3:
        lines.append("- oracle_recall_at_k 也偏低：当前触发点太晚，或候选集合本身缺少未来 demand page。")
        diagnostics_added = True
    if max_oracle >= 0.5 and max_xgboost < max_oracle * 0.6:
        lines.append("- oracle 明显高于 xgboost：特征、训练标签或模型排序可能有问题。")
        diagnostics_added = True
    if max_xgboost >= max_oracle * 0.8 and max_oracle >= 0.5:
        lines.append("- replay xgboost 接近 oracle：如果 online xgboost 没收益，优先检查在线限流、I/O depth、ready 时机或 trace 时间戳。")
        diagnostics_added = True
    if not diagnostics_added:
        lines.append("- 未发现明显 replay 异常。")
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description="Replay XGBoost prefetch policy from trace CSV.")
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--top-k", nargs="+", type=int, required=True)
    parser.add_argument("--out-json", type=Path)
    parser.add_argument("--out-md", type=Path)
    args = parser.parse_args()

    if any(k <= 0 for k in args.top_k):
        raise SystemExit("--top-k values must be positive")

    rows = load_trace(args.trace)
    groups = group_rows(rows)
    base_score, trees = load_xgboost_dump(args.model)

    raw_results = []
    oracle_by_k = {}
    scorers = {
        "xgboost": lambda row: xgboost_score(row, base_score, trees),
        "oracle": oracle_score,
    }
    for top_k in args.top_k:
        oracle_by_k[top_k] = replay(groups, scorers["oracle"], top_k)
    for top_k in args.top_k:
        for policy in ("xgboost", "oracle"):
            metrics = replay(groups, scorers[policy], top_k)
            oracle_recall = oracle_by_k[top_k]["recall_at_k"]
            recall = metrics["recall_at_k"]
            metrics.update(
                {
                    "policy": policy,
                    "top_k": top_k,
                    "oracle_gap": None
                    if oracle_recall is None or recall is None
                    else oracle_recall - recall,
                }
            )
            raw_results.append(metrics)

    payload = {
        "trace": str(args.trace),
        "model": str(args.model) if args.model else None,
        "top_k": args.top_k,
        "results": raw_results,
    }
    text = json.dumps(payload, indent=2, sort_keys=True)
    if args.out_json:
        args.out_json.parent.mkdir(parents=True, exist_ok=True)
        args.out_json.write_text(text + "\n", encoding="utf-8")
    if args.out_md:
        args.out_md.parent.mkdir(parents=True, exist_ok=True)
        args.out_md.write_text(make_markdown(payload), encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
