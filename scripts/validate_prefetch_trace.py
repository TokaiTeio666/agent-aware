#!/usr/bin/env python3
"""Validate ownership and metric invariants in one prefetch trace run."""

from __future__ import annotations

import argparse
import json
from collections import defaultdict
from pathlib import Path

from prefetch_trace_utils import (
    TERMINAL_OUTCOMES,
    TraceAccumulator,
    is_missing_at_decision,
    iter_trace_rows,
    parse_bool,
    trace_outcome,
)


REQUIRED_COLUMNS = {
    "run_id",
    "policy_version",
    "seed",
    "query_id",
    "step_id",
    "trigger",
    "decision_id",
    "submission_id",
    "candidate_page_id",
    "selected_by_policy",
    "was_prefetched",
    "was_demanded",
    "hit_type",
    "saved_wait_us",
}


def main() -> None:
    parser = argparse.ArgumentParser(description="Validate a prefetch trace run.")
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--result-json", type=Path)
    parser.add_argument("--json-out", type=Path)
    args = parser.parse_args()

    errors = []
    accumulator = TraceAccumulator()
    by_trigger = defaultdict(TraceAccumulator)
    submission_owners = set()
    submitted_pages = set()
    columns = None

    for row in iter_trace_rows([args.trace]):
        if columns is None:
            columns = set(row) - {"_trace_file"}
            missing = sorted(REQUIRED_COLUMNS - columns)
            if missing:
                errors.append("missing required columns: " + ", ".join(missing))
        accumulator.add(row)
        by_trigger[str(row.get("trigger") or "unknown")].add(row)

        selected = bool(parse_bool(row.get("selected_by_policy")))
        demanded = bool(parse_bool(row.get("was_demanded")))
        if (
            str(row.get("trigger") or "") == "rerank"
            and selected
            and is_missing_at_decision(row)
            and not demanded
        ):
            errors.append(
                "selected missing rerank page was not demanded: "
                f"query={row.get('query_id')} page={row.get('candidate_page_id')}"
            )

        if not bool(parse_bool(row.get("was_prefetched"))):
            continue
        run_id = str(row.get("run_id") or "")
        query_id = str(row.get("query_id") or "")
        submission_id = str(row.get("submission_id") or "")
        page_id = str(row.get("candidate_page_id") or "")
        owner_key = (run_id, query_id, submission_id)
        page_key = (run_id, query_id, page_id)
        if not submission_id or submission_id == "0":
            errors.append(f"submitted row has no submission_id: {page_key}")
        elif owner_key in submission_owners:
            errors.append(f"duplicate submission owner: {owner_key}")
        else:
            submission_owners.add(owner_key)
        if page_key in submitted_pages:
            errors.append(f"page submitted more than once in query: {page_key}")
        else:
            submitted_pages.add(page_key)
        if trace_outcome(row) not in TERMINAL_OUTCOMES:
            errors.append(f"submitted row has invalid terminal outcome: {owner_key}")

    summary = accumulator.finish([str(args.trace)])
    trigger_summary = {
        name: value.finish([str(args.trace)])
        for name, value in sorted(by_trigger.items())
    }
    if summary["rows"] == 0:
        errors.append("trace contains no rows")
    if len(summary["run_ids"]) != 1:
        errors.append(f"expected one run_id, found {summary['run_ids']}")
    if len(summary["policy_versions"]) != 1:
        errors.append(
            f"expected one policy_version, found {summary['policy_versions']}"
        )
    if len(summary["seeds"]) != 1:
        errors.append(f"expected one seed, found {summary['seeds']}")
    if not summary["useful_identity_ok"]:
        errors.append("useful != ready_hit + pending_hit")
    if not summary["terminal_identity_ok"]:
        errors.append("submitted != useful + wasted")

    if args.result_json:
        result = json.loads(args.result_json.read_text(encoding="utf-8"))
        stats = result.get("stats", {})
        expected = {
            "submitted": stats.get("prefetch_submitted"),
            "ready_hit": stats.get("prefetch_ready_hit"),
            "pending_hit": stats.get("prefetch_pending_hit"),
            "useful": stats.get("prefetch_useful_pages"),
            "wasted": stats.get("prefetch_wasted_pages"),
        }
        for name, value in expected.items():
            if value is not None and summary[name] != value:
                errors.append(
                    f"trace/result mismatch for {name}: "
                    f"trace={summary[name]} result={value}"
                )

    payload = {
        "status": "ok" if not errors else "failed",
        "trace": str(args.trace),
        "summary": summary,
        "by_trigger": trigger_summary,
        "errors": errors,
    }
    text = json.dumps(payload, indent=2, sort_keys=True)
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(text + "\n", encoding="utf-8")
    print(text)
    if errors:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
