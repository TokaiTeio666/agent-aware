#!/usr/bin/env python3
"""Streaming offline replay for XGBoost and oracle prefetch policies."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Callable, Dict, Iterator, List, Sequence

from prefetch_trace import (
    is_missing_at_decision,
    iter_trace_rows,
    parse_bool,
    parse_float,
    trace_group_id,
    trace_outcome,
)
from train_prefetch_ranker import TRACE_FEATURES


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


def feature_value(row: dict, feature_name: str) -> float:
    for trace_name, model_name in TRACE_FEATURES:
        if model_name == feature_name:
            return parse_float(row.get(trace_name)) or 0.0
    return 0.0


def xgboost_score(
    row: dict, base_score: float, trees: List[Dict[int, TreeNode]]
) -> float:
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


def is_positive(row: dict) -> bool:
    return trace_outcome(row) in {"ready_hit", "pending_hit"} or bool(
        parse_bool(row.get("was_demanded"))
    )


def oracle_score(row: dict) -> float:
    rank = parse_float(row.get("min_pq_rank_on_page")) or 0.0
    return (1.0 if is_positive(row) else 0.0) * 10000.0 - rank


def iter_groups(path: Path) -> Iterator[tuple[str, List[dict]]]:
    current_group = None
    rows: List[dict] = []
    for row in iter_trace_rows([path]):
        group_id = trace_group_id(row)
        if current_group is None:
            current_group = group_id
        if group_id != current_group:
            yield current_group, rows
            current_group = group_id
            rows = []
        rows.append(row)
    if current_group is not None:
        yield current_group, rows


class ReplayAccumulator:
    def __init__(self) -> None:
        self.groups = 0
        self.selected_pages = 0
        self.positive_pages = 0
        self.selected_positive_pages = 0

    def add(self, candidates: Sequence[dict], selected: Sequence[dict]) -> None:
        self.groups += 1
        self.selected_pages += len(selected)
        self.positive_pages += sum(1 for row in candidates if is_positive(row))
        self.selected_positive_pages += sum(
            1 for row in selected if is_positive(row)
        )

    def finish(self) -> dict:
        useful_rate = (
            self.selected_positive_pages / self.selected_pages
            if self.selected_pages
            else None
        )
        recall = (
            self.selected_positive_pages / self.positive_pages
            if self.positive_pages
            else None
        )
        return {
            "groups": self.groups,
            "selected_pages": self.selected_pages,
            "positive_pages": self.positive_pages,
            "selected_positive_pages": self.selected_positive_pages,
            "useful_rate": useful_rate,
            "precision_at_k": useful_rate,
            "recall_at_k": recall,
        }


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Replay XGBoost prefetch policy from a trace CSV."
    )
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--top-k", nargs="+", type=int, required=True)
    parser.add_argument("--match-online-budget", action="store_true")
    parser.add_argument("--out-json", type=Path)
    args = parser.parse_args()

    if any(value <= 0 for value in args.top_k):
        raise SystemExit("--top-k values must be positive")

    base_score, trees = load_xgboost_dump(args.model)
    scorers: Dict[str, Callable[[dict], float]] = {
        "xgboost": lambda row: xgboost_score(row, base_score, trees),
        "oracle": oracle_score,
    }
    accumulators = {
        (policy, top_k): ReplayAccumulator()
        for top_k in args.top_k
        for policy in scorers
    }
    online_submitted = 0
    online_useful = 0

    for _, group in iter_groups(args.trace):
        online_rows = [
            row for row in group if bool(parse_bool(row.get("was_prefetched")))
        ]
        online_submitted += len(online_rows)
        online_useful += sum(
            1
            for row in online_rows
            if trace_outcome(row) in {"ready_hit", "pending_hit"}
        )
        candidates = [row for row in group if is_missing_at_decision(row)]
        for policy, scorer in scorers.items():
            ranked = sorted(candidates, key=scorer, reverse=True)
            for top_k in args.top_k:
                budget = len(online_rows) if args.match_online_budget else top_k
                accumulators[(policy, top_k)].add(candidates, ranked[:budget])

    online_useful_rate = (
        online_useful / online_submitted if online_submitted else None
    )
    results = []
    for top_k in args.top_k:
        for policy in ("xgboost", "oracle"):
            metrics = accumulators[(policy, top_k)].finish()
            useful_rate = metrics["useful_rate"]
            evaluable = (
                online_submitted > 0
                and online_useful > 0
                and metrics["positive_pages"] > 0
                and metrics["selected_pages"] > 0
            )
            metrics.update(
                {
                    "policy": policy,
                    "top_k": top_k,
                    "evaluable": evaluable,
                    "match_online_budget": args.match_online_budget,
                    "online_submitted_pages": online_submitted,
                    "online_useful_pages": online_useful,
                    "online_useful_rate": online_useful_rate,
                    "useful_rate_gap_pp": (
                        None
                        if not evaluable
                        or useful_rate is None
                        or online_useful_rate is None
                        else (useful_rate - online_useful_rate) * 100.0
                    ),
                    "absolute_useful_rate_gap_pp": (
                        None
                        if not evaluable
                        or useful_rate is None
                        or online_useful_rate is None
                        else abs(useful_rate - online_useful_rate) * 100.0
                    ),
                }
            )
            results.append(metrics)

    payload = {
        "trace": str(args.trace),
        "model": str(args.model),
        "top_k": args.top_k,
        "match_online_budget": args.match_online_budget,
        "replay_evaluable": bool(results)
        and all(row["evaluable"] for row in results),
        "results": results,
    }
    text = json.dumps(payload, indent=2, sort_keys=True)
    if args.out_json:
        args.out_json.parent.mkdir(parents=True, exist_ok=True)
        args.out_json.write_text(text + "\n", encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
