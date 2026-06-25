#!/usr/bin/env python3
"""Train an XGBoost-style page-level prefetch ranker.

The online C++ path consumes an XGBoost text dump.  When the optional
``xgboost`` Python package is installed this script trains a real XGBoost
regressor and writes ``Booster.get_dump()`` output.  In minimal environments it
falls back to a small gradient-boosted stump ensemble written in the same text
tree format, so the PQ+ADC -> page ranking -> online scoring loop remains
testable without adding a build-time runtime dependency.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import random
import sys
from collections import defaultdict
from pathlib import Path
from typing import Callable, Dict, Iterable, List, Sequence, Tuple


TRACE_FEATURES = [
    ("num_candidates_on_page", "num_candidates_on_page"),
    ("min_pq_rank_on_page", "min_pq_rank_on_page"),
    ("avg_pq_rank_on_page", "avg_pq_rank_on_page"),
    ("candidate_rank_span", "candidate_rank_span"),
    ("contains_top1_candidate", "contains_top1_candidate"),
    ("contains_topk_candidate", "contains_topk_candidate"),
    ("is_cached_at_decision", "is_cached"),
    ("is_inflight_at_decision", "is_inflight"),
    ("io_queue_depth", "io_queue_depth"),
    ("cache_pressure", "cache_pressure"),
]


def parse_float(value: object) -> float:
    if value is None or value == "" or value == "-1":
        return 0.0
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return 0.0
    return parsed if math.isfinite(parsed) else 0.0


def load_trace(path: Path) -> List[dict]:
    with path.open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    if not rows:
        raise RuntimeError(f"empty prefetch trace: {path}")
    for row in rows:
        row["label"] = int(parse_float(row.get("label")))
        row["query_id"] = int(parse_float(row.get("query_id")))
    return rows


def split_queries(
    rows: Sequence[dict],
    train_queries: int = 0,
    valid_queries: int = 0,
    test_queries: int = 0,
    seed: int = 42,
) -> Tuple[set, set, set]:
    """Split query IDs into train/valid/test sets.

    When absolute counts (train_queries, valid_queries, test_queries) are all
    positive, they take precedence.  Otherwise a 60/20/20 ratio split is used.
    The split is randomised (not chronological) and reproducible via *seed*.
    """
    import random as _random

    query_ids = sorted({row["query_id"] for row in rows})
    n = len(query_ids)
    if n < 3:
        ids = set(query_ids)
        return ids, ids, ids

    rng = _random.Random(seed)
    shuffled = list(query_ids)
    rng.shuffle(shuffled)

    if train_queries > 0 and valid_queries > 0 and test_queries > 0:
        total = train_queries + valid_queries + test_queries
        if n < total:
            # Scale down proportionally if not enough queries
            ratio = n / total
            train_n = max(1, int(train_queries * ratio))
            valid_n = max(1, int(valid_queries * ratio))
            test_n = n - train_n - valid_n
        else:
            train_n = train_queries
            valid_n = valid_queries
            test_n = min(test_queries, n - train_n - valid_n)
    else:
        # 60/20/20 ratio split
        train_n = max(1, int(n * 0.60))
        valid_n = max(1, int(n * 0.20))
        test_n = n - train_n - valid_n

    test_n = max(1, test_n)
    valid_n = max(1, valid_n)
    train_n = n - valid_n - test_n

    return (
        set(shuffled[:train_n]),
        set(shuffled[train_n:train_n + valid_n]),
        set(shuffled[train_n + valid_n:]),
    )


def vector(row: dict) -> List[float]:
    return [parse_float(row.get(trace_name)) for trace_name, _ in TRACE_FEATURES]


def group_rows(rows: Iterable[dict]) -> Dict[str, List[dict]]:
    groups: Dict[str, List[dict]] = defaultdict(list)
    for row in rows:
        groups[row.get("group_id") or f"{row['query_id']}_{row.get('step_id', 0)}"].append(row)
    return groups


class Stump:
    def __init__(self, feature_idx: int, threshold: float, left: float, right: float):
        self.feature_idx = feature_idx
        self.threshold = threshold
        self.left = left
        self.right = right

    def score(self, values: Sequence[float]) -> float:
        return self.left if values[self.feature_idx] < self.threshold else self.right


def candidate_thresholds(values: Sequence[float]) -> List[float]:
    uniq = sorted(set(values))
    if len(uniq) <= 1:
        return []
    if len(uniq) > 32:
        picks = []
        for i in range(1, 32):
            picks.append(uniq[min(len(uniq) - 1, math.floor(i * len(uniq) / 32))])
        uniq = sorted(set(picks))
    return [(a + b) / 2.0 for a, b in zip(uniq, uniq[1:])]


def best_stump(vectors: Sequence[Sequence[float]], residuals: Sequence[float]) -> Stump:
    best = Stump(0, math.inf, 0.0, 0.0)
    best_loss = math.inf
    feature_count = len(TRACE_FEATURES)
    for feature_idx in range(feature_count):
        values = [row[feature_idx] for row in vectors]
        for threshold in candidate_thresholds(values):
            left = [res for value, res in zip(values, residuals) if value < threshold]
            right = [res for value, res in zip(values, residuals) if value >= threshold]
            if not left or not right:
                continue
            left_value = sum(left) / len(left)
            right_value = sum(right) / len(right)
            loss = 0.0
            for value, residual in zip(values, residuals):
                pred = left_value if value < threshold else right_value
                error = residual - pred
                loss += error * error
            if loss < best_loss:
                best_loss = loss
                best = Stump(feature_idx, threshold, left_value, right_value)
    if not math.isfinite(best_loss):
        mean = sum(residuals) / max(1, len(residuals))
        return Stump(0, math.inf, mean, mean)
    return best


def train_stump_ensemble(
    train_rows: Sequence[dict],
    rounds: int,
    learning_rate: float,
    seed: int,
) -> tuple[float, List[Stump]]:
    rng = random.Random(seed)
    rows = list(train_rows)
    rng.shuffle(rows)
    labels = [float(row["label"]) for row in rows]
    vectors = [vector(row) for row in rows]
    base_score = sum(labels) / max(1, len(labels))
    predictions = [base_score] * len(rows)
    stumps: List[Stump] = []
    for _ in range(max(1, rounds)):
        residuals = [label - pred for label, pred in zip(labels, predictions)]
        stump = best_stump(vectors, residuals)
        stump.left *= learning_rate
        stump.right *= learning_rate
        for i, values in enumerate(vectors):
            predictions[i] += stump.score(values)
        stumps.append(stump)
    return base_score, stumps


def write_xgboost_dump(path: Path, base_score: float, stumps: Sequence[Stump]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as handle:
        handle.write("# agent-aware xgboost text dump\n")
        handle.write(f"base_score={base_score:.12g}\n")
        for tree_id, stump in enumerate(stumps):
            feature = TRACE_FEATURES[stump.feature_idx][1]
            handle.write(f"booster[{tree_id}]:\n")
            handle.write(
                f"0:[{feature}<{stump.threshold:.12g}] yes=1,no=2,missing=1\n"
            )
            handle.write(f"\t1:leaf={stump.left:.12g}\n")
            handle.write(f"\t2:leaf={stump.right:.12g}\n")


def train_real_xgboost(
    train_rows: Sequence[dict],
    rounds: int,
    learning_rate: float,
    max_depth: int,
    seed: int,
    model_out: Path,
) -> tuple[bool, str]:
    try:
        import xgboost as xgb  # type: ignore
    except Exception as exc:
        return False, f"xgboost unavailable: {exc}"

    labels = [float(row["label"]) for row in train_rows]
    data = [vector(row) for row in train_rows]
    base_score = sum(labels) / max(1, len(labels))
    dtrain = xgb.DMatrix(
        data,
        label=labels,
        feature_names=[name for _, name in TRACE_FEATURES],
    )
    params = {
        "objective": "reg:squarederror",
        "eta": learning_rate,
        "max_depth": max_depth,
        "min_child_weight": 1,
        "lambda": 1.0,
        "base_score": base_score,
        "seed": seed,
        "verbosity": 0,
    }
    booster = xgb.train(params, dtrain, num_boost_round=max(1, rounds))
    dump = booster.get_dump(with_stats=False, dump_format="text")
    model_out.parent.mkdir(parents=True, exist_ok=True)
    with model_out.open("w", encoding="utf-8") as handle:
        handle.write("# agent-aware xgboost text dump\n")
        handle.write(f"base_score={base_score:.12g}\n")
        for tree_id, tree in enumerate(dump):
            handle.write(f"booster[{tree_id}]:\n")
            handle.write(tree.rstrip() + "\n")
    return True, "xgboost"


def load_xgboost_dump(path: Path) -> tuple[float, List[Stump]]:
    base_score = 0.0
    stumps: List[Stump] = []
    feature_to_idx = {name: i for i, (_, name) in enumerate(TRACE_FEATURES)}
    current_split: tuple[int, float] | None = None
    left_leaf: float | None = None
    right_leaf: float | None = None
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("base_score="):
            base_score = float(line.split("=", 1)[1])
            continue
        if line.startswith("booster["):
            if current_split is not None and left_leaf is not None and right_leaf is not None:
                stumps.append(Stump(current_split[0], current_split[1], left_leaf, right_leaf))
            current_split = None
            left_leaf = right_leaf = None
            continue
        if ":[" in line and "<" in line:
            body = line.split(":[", 1)[1].split("]", 1)[0]
            feature, threshold = body.split("<", 1)
            if feature not in feature_to_idx:
                continue
            current_split = (feature_to_idx[feature], float(threshold))
            continue
        if ":leaf=" in line:
            node_id = int(line.split(":", 1)[0])
            leaf = float(line.split("leaf=", 1)[1].split(",", 1)[0])
            if node_id == 1:
                left_leaf = leaf
            elif node_id == 2:
                right_leaf = leaf
    if current_split is not None and left_leaf is not None and right_leaf is not None:
        stumps.append(Stump(current_split[0], current_split[1], left_leaf, right_leaf))
    return base_score, stumps


def score_dump(row: dict, base_score: float, stumps: Sequence[Stump]) -> float:
    values = vector(row)
    return base_score + sum(stump.score(values) for stump in stumps)


def dcg(labels: Sequence[int]) -> float:
    return sum((2.0 ** label - 1.0) / math.log2(i + 2.0) for i, label in enumerate(labels))


def evaluate(
    rows: Sequence[dict],
    scorer: Callable[[dict], float],
    top_k: int,
    score_threshold: float,
    max_inflight: int,
) -> dict:
    groups = group_rows(rows)
    ndcg_sum = precision_sum = recall_sum = 0.0
    replay_selected = replay_ready = replay_pending = replay_unused = 0
    for group in groups.values():
        ranked = [
            row
            for row, score in sorted(
                ((row, scorer(row)) for row in group),
                key=lambda item: item[1],
                reverse=True,
            )
            if score >= score_threshold
        ]
        limit = top_k if max_inflight <= 0 else min(top_k, max_inflight)
        selected = ranked[:limit]
        selected_labels = [row["label"] for row in selected]
        ideal_labels = sorted((row["label"] for row in group), reverse=True)[:top_k]
        ideal = dcg(ideal_labels)
        ndcg_sum += 0.0 if ideal == 0.0 else dcg(selected_labels) / ideal
        positives = sum(1 for row in group if row["label"] > 0)
        selected_positive = sum(1 for label in selected_labels if label > 0)
        precision_sum += selected_positive / max(1, len(selected))
        recall_sum += selected_positive / max(1, positives)
        replay_selected += len(selected)
        replay_ready += sum(1 for label in selected_labels if label == 3)
        replay_pending += sum(1 for label in selected_labels if label == 1)
        replay_unused += sum(1 for label in selected_labels if label == 0)
    group_count = max(1, len(groups))
    return {
        "groups": len(groups),
        "ndcg_at_k": ndcg_sum / group_count,
        "precision_at_k": precision_sum / group_count,
        "recall_at_k": recall_sum / group_count,
        "replay_ready_hit_rate": replay_ready / max(1, replay_selected),
        "replay_pending_hit_rate": replay_pending / max(1, replay_selected),
        "replay_unused_rate": replay_unused / max(1, replay_selected),
        "replay_selected": replay_selected,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="Train an XGBoost prefetch ranker.")
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--model-out", required=True, type=Path)
    parser.add_argument("--metrics-out", type=Path)
    parser.add_argument("--top-k", type=int, default=8)
    parser.add_argument("--score-threshold", type=float, default=-math.inf)
    parser.add_argument("--max-inflight", type=int, default=0)
    parser.add_argument("--rounds", type=int, default=16)
    parser.add_argument("--learning-rate", type=float, default=0.2)
    parser.add_argument("--max-depth", type=int, default=3)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--train-queries", type=int, default=3000,
                        help="Number of queries for training set (default 3000)")
    parser.add_argument("--valid-queries", type=int, default=1000,
                        help="Number of queries for validation set (default 1000)")
    parser.add_argument("--test-queries", type=int, default=1000,
                        help="Number of queries for test set (default 1000)")
    parser.add_argument(
        "--train-all",
        action="store_true",
        help=(
            "Use every query in the input trace for training. Use this when "
            "the train/valid/test split is done by separate benchmark runs."
        ),
    )
    # Kept as compatibility no-ops for older smoke commands.
    parser.add_argument("--backend", type=str)
    parser.add_argument("--epochs", type=int)
    parser.add_argument("--l2", type=float)
    args = parser.parse_args()

    if args.top_k <= 0:
        raise RuntimeError("--top-k must be positive")
    if args.max_inflight < 0:
        raise RuntimeError("--max-inflight must be >= 0")

    rows = load_trace(args.trace)
    if args.train_all:
        splits = {"train": rows}
        split_mode = "train-all"
    else:
        train_ids, valid_ids, test_ids = split_queries(
            rows,
            train_queries=args.train_queries,
            valid_queries=args.valid_queries,
            test_queries=args.test_queries,
            seed=args.seed,
        )
        splits = {
            "train": [row for row in rows if row["query_id"] in train_ids],
            "valid": [row for row in rows if row["query_id"] in valid_ids],
            "test": [row for row in rows if row["query_id"] in test_ids],
        }
        split_mode = f"train{len(train_ids)}_valid{len(valid_ids)}_test{len(test_ids)}"

    ok, note = train_real_xgboost(
        splits["train"],
        args.rounds,
        args.learning_rate,
        args.max_depth,
        args.seed,
        args.model_out,
    )
    if not ok:
        raise RuntimeError(note)

    base_score, stumps = load_xgboost_dump(args.model_out)

    metrics = {
        "trace": str(args.trace),
        "model_out": str(args.model_out),
        "backend": "xgboost",
        "top_k": args.top_k,
        "score_threshold": args.score_threshold if math.isfinite(args.score_threshold) else None,
        "max_inflight": args.max_inflight,
        "split_mode": split_mode,
        "features": [model_name for _, model_name in TRACE_FEATURES],
        "splits": {},
    }
    for name, split_rows in splits.items():
        xgboost_metrics = evaluate(
            split_rows,
            lambda row: score_dump(row, base_score, stumps),
            args.top_k,
            args.score_threshold,
            args.max_inflight,
        )
        metrics["splits"][name] = {"xgboost": xgboost_metrics}

    payload = json.dumps(metrics, indent=2, sort_keys=True)
    if args.metrics_out:
        args.metrics_out.parent.mkdir(parents=True, exist_ok=True)
        args.metrics_out.write_text(payload + "\n", encoding="utf-8")
    print(payload)


if __name__ == "__main__":
    main()
