#!/usr/bin/env python3
"""Streaming helpers for prefetch trace CSV files."""

from __future__ import annotations

import csv
import math
from pathlib import Path
from typing import Iterable, Iterator, Optional, Sequence


USEFUL_OUTCOMES = {"ready_hit", "pending_hit"}
TERMINAL_OUTCOMES = USEFUL_OUTCOMES | {"wasted"}


def parse_bool(value: object) -> Optional[bool]:
    if value is None:
        return None
    text = str(value).strip().lower()
    if text in {"", "null", "none", "nan"}:
        return None
    if text in {"1", "true", "yes", "y", "on"}:
        return True
    if text in {"0", "false", "no", "n", "off", "-1"}:
        return False
    return None


def parse_float(value: object) -> Optional[float]:
    if value is None:
        return None
    text = str(value).strip()
    if text in {"", "-1", "null", "None", "nan"}:
        return None
    try:
        parsed = float(text)
    except ValueError:
        return None
    return parsed if math.isfinite(parsed) else None


def parse_int(value: object) -> Optional[int]:
    parsed = parse_float(value)
    return int(parsed) if parsed is not None else None


def trace_outcome(row: dict) -> str:
    explicit = str(row.get("hit_type") or "").strip()
    if explicit in TERMINAL_OUTCOMES:
        return explicit
    if not bool(parse_bool(row.get("was_prefetched"))):
        return ""
    if bool(parse_bool(row.get("was_demanded"))):
        if bool(parse_bool(row.get("was_ready_before_demand"))):
            return "ready_hit"
        return "pending_hit"
    return "wasted"


def trace_group_id(row: dict) -> str:
    group_id = str(row.get("group_id") or "").strip()
    if group_id:
        return group_id
    run_id = str(row.get("run_id") or "legacy")
    return f"{run_id}:{row.get('query_id', 'unknown')}:{row.get('step_id', 0)}"


def is_missing_at_decision(row: dict) -> bool:
    return not any(
        bool(parse_bool(row.get(name)))
        for name in (
            "is_cached_at_decision",
            "is_inflight_at_decision",
            "is_ready_at_decision",
            "is_materialized_at_decision",
        )
    )


def iter_trace_rows(paths: Sequence[Path]) -> Iterator[dict]:
    for path in paths:
        with path.open(newline="", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)
            if reader.fieldnames is None:
                raise RuntimeError(f"trace has no header: {path}")
            for row in reader:
                row["_trace_file"] = str(path)
                yield row


class TraceAccumulator:
    def __init__(self) -> None:
        self.rows = 0
        self.query_ids = set()
        self.run_ids = set()
        self.policy_versions = set()
        self.seeds = set()
        self.submitted = 0
        self.ready_hit = 0
        self.pending_hit = 0
        self.useful = 0
        self.wasted = 0
        self.saved_wait_us_sum = 0.0

    def add(self, row: dict) -> None:
        self.rows += 1
        query_id = str(row.get("query_id") or "").strip()
        if query_id:
            self.query_ids.add(query_id)
        run_id = str(row.get("run_id") or "").strip()
        if run_id:
            self.run_ids.add(run_id)
        policy_version = str(row.get("policy_version") or "").strip()
        if policy_version:
            self.policy_versions.add(policy_version)
        seed = str(row.get("seed") or "").strip()
        if seed:
            self.seeds.add(seed)

        if not bool(parse_bool(row.get("was_prefetched"))):
            return
        self.submitted += 1
        outcome = trace_outcome(row)
        if outcome == "ready_hit":
            self.ready_hit += 1
            self.useful += 1
        elif outcome == "pending_hit":
            self.pending_hit += 1
            self.useful += 1
        else:
            self.wasted += 1
        saved_wait = parse_float(row.get("saved_wait_us"))
        if saved_wait is not None and saved_wait > 0:
            self.saved_wait_us_sum += saved_wait

    def finish(self, trace_files: Optional[Iterable[str]] = None) -> dict:
        def rate(numerator: int) -> Optional[float]:
            return numerator / self.submitted if self.submitted else None

        return {
            "trace_files": sorted(set(trace_files or [])),
            "rows": self.rows,
            "queries": len(self.query_ids),
            "run_ids": sorted(self.run_ids),
            "policy_versions": sorted(self.policy_versions),
            "seeds": sorted(self.seeds),
            "submitted": self.submitted,
            "ready_hit": self.ready_hit,
            "pending_hit": self.pending_hit,
            "useful": self.useful,
            "wasted": self.wasted,
            "saved_wait_us_sum": self.saved_wait_us_sum,
            "saved_wait_us_avg": (
                self.saved_wait_us_sum / self.useful if self.useful else None
            ),
            "useful_rate": rate(self.useful),
            "ready_hit_rate": rate(self.ready_hit),
            "pending_hit_rate": rate(self.pending_hit),
            "waste_rate": rate(self.wasted),
            "useful_identity_ok": self.useful
            == self.ready_hit + self.pending_hit,
            "terminal_identity_ok": self.submitted
            == self.useful + self.wasted,
        }
