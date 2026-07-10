#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import math
from collections import defaultdict
from pathlib import Path

USEFUL_OUTCOMES = {"ready_hit", "pending_hit"}
TERMINAL_OUTCOMES = USEFUL_OUTCOMES | {"wasted"}
REQUIRED_COLUMNS = {
    "run_id", "policy_version", "seed", "query_id", "step_id",
    "first_seen_step", "estimated_demand_step", "trigger", "group_id",
    "decision_id", "submission_id", "candidate_page_id", "selected_by_policy",
    "was_prefetched", "was_demanded", "hit_type", "saved_wait_us",
}


def parse_bool(value: object) -> bool | None:
    text = str(value).strip().lower() if value is not None else ""
    if text in {"1", "true", "yes", "y", "on"}:
        return True
    if text in {"0", "false", "no", "n", "off", "-1"}:
        return False
    return None


def parse_float(value: object) -> float | None:
    try:
        parsed = float(str(value).strip())
    except (TypeError, ValueError):
        return None
    return parsed if parsed != -1 and math.isfinite(parsed) else None


def parse_int(value: object) -> int | None:
    parsed = parse_float(value)
    return int(parsed) if parsed is not None else None


def trace_outcome(row: dict) -> str:
    explicit = str(row.get("hit_type") or "").strip()
    if explicit in TERMINAL_OUTCOMES:
        return explicit
    if not parse_bool(row.get("was_prefetched")):
        return ""
    if parse_bool(row.get("was_demanded")):
        return "ready_hit" if parse_bool(row.get("was_ready_before_demand")) else "pending_hit"
    return "wasted"


def trace_group_id(row: dict) -> str:
    return str(row.get("group_id") or
               f"{row.get('run_id', 'legacy')}:{row.get('query_id', 'unknown')}:{row.get('step_id', 0)}")


def is_missing_at_decision(row: dict) -> bool:
    return not any(parse_bool(row.get(name)) for name in (
        "is_cached_at_decision", "is_inflight_at_decision",
        "is_ready_at_decision", "is_materialized_at_decision"))


def iter_trace_rows(paths: list[Path]):
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
        self.rows = self.submitted = self.ready_hit = self.pending_hit = 0
        self.useful = self.wasted = 0
        self.saved_wait_us_sum = 0.0
        self.query_ids, self.run_ids, self.policy_versions, self.seeds = set(), set(), set(), set()

    def add(self, row: dict) -> None:
        self.rows += 1
        for field, target in (("query_id", self.query_ids), ("run_id", self.run_ids),
                              ("policy_version", self.policy_versions), ("seed", self.seeds)):
            value = str(row.get(field) or "").strip()
            if value:
                target.add(value)
        if not parse_bool(row.get("was_prefetched")):
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
        saved = parse_float(row.get("saved_wait_us"))
        if saved is not None and saved > 0:
            self.saved_wait_us_sum += saved

    def finish(self, trace_files=()) -> dict:
        rate = lambda value: value / self.submitted if self.submitted else None
        return {
            "trace_files": sorted(set(trace_files)), "rows": self.rows,
            "queries": len(self.query_ids), "run_ids": sorted(self.run_ids),
            "policy_versions": sorted(self.policy_versions), "seeds": sorted(self.seeds),
            "submitted": self.submitted, "ready_hit": self.ready_hit,
            "pending_hit": self.pending_hit, "useful": self.useful, "wasted": self.wasted,
            "saved_wait_us_sum": self.saved_wait_us_sum,
            "saved_wait_us_avg": self.saved_wait_us_sum / self.useful if self.useful else None,
            "useful_rate": rate(self.useful), "ready_hit_rate": rate(self.ready_hit),
            "pending_hit_rate": rate(self.pending_hit), "waste_rate": rate(self.wasted),
            "useful_identity_ok": self.useful == self.ready_hit + self.pending_hit,
            "terminal_identity_ok": self.submitted == self.useful + self.wasted,
        }


def write_payload(payload: dict, json_out: Path | None) -> None:
    text = json.dumps(payload, indent=2, sort_keys=True)
    if json_out:
        json_out.parent.mkdir(parents=True, exist_ok=True)
        json_out.write_text(text + "\n", encoding="utf-8")
    print(text)


def analyze(args: argparse.Namespace) -> None:
    names = [str(path) for path in args.trace]
    total, triggers, groups = TraceAccumulator(), defaultdict(TraceAccumulator), defaultdict(TraceAccumulator)
    for row in iter_trace_rows(args.trace):
        total.add(row)
        trigger = str(row.get("trigger") or "unknown")
        triggers[trigger].add(row)
        if args.group_by == "query":
            groups[str(row.get("query_id") or "unknown")].add(row)
        elif args.group_by == "trigger":
            groups[trigger].add(row)
    if not total.rows:
        raise SystemExit("no trace rows found")
    payload = {"global": total.finish(names),
               "by_trigger": {key: value.finish(names) for key, value in sorted(triggers.items())}}
    if groups:
        payload["groups"] = {key: value.finish(names) for key, value in sorted(groups.items())}
    write_payload(payload, args.json_out)


def validate(args: argparse.Namespace) -> None:
    errors, total, triggers = [], TraceAccumulator(), defaultdict(TraceAccumulator)
    owners, pages, selected = set(), set(), defaultdict(int)
    trigger_by_group, post_expand, warmups = {}, defaultdict(set), defaultdict(set)
    columns = None
    for row in iter_trace_rows([args.trace]):
        if columns is None:
            columns = set(row) - {"_trace_file"}
            if REQUIRED_COLUMNS - columns:
                errors.append("missing required columns: " + ", ".join(sorted(REQUIRED_COLUMNS - columns)))
        total.add(row)
        trigger = str(row.get("trigger") or "unknown")
        triggers[trigger].add(row)
        group, page = str(row.get("group_id") or ""), str(row.get("candidate_page_id") or "")
        trigger_by_group[group] = trigger
        chosen, demanded = bool(parse_bool(row.get("selected_by_policy"))), bool(parse_bool(row.get("was_demanded")))
        if chosen:
            selected[group] += 1
        if trigger == "post_expand":
            post_expand[group].add(page)
        if trigger == "warmup_entry":
            warmups[str(row.get("query_id") or "")].add(group)
        step, first, demand = map(parse_int, (row.get("step_id"), row.get("first_seen_step"), row.get("estimated_demand_step")))
        if step is not None and first is not None and first > step:
            errors.append(f"first_seen_step is after decision step: query={row.get('query_id')} page={page}")
        if trigger == "post_expand" and chosen and step is not None and (demand is None or demand < step + 1):
            errors.append(f"selected post_expand page is too late: query={row.get('query_id')} page={page}")
        if trigger == "rerank" and chosen and is_missing_at_decision(row) and not demanded:
            errors.append(f"selected missing rerank page was not demanded: query={row.get('query_id')} page={page}")
        if not parse_bool(row.get("was_prefetched")):
            continue
        owner = (str(row.get("run_id") or ""), str(row.get("query_id") or ""), str(row.get("submission_id") or ""))
        page_key = owner[:2] + (page,)
        if owner[2] in {"", "0"}:
            errors.append(f"submitted row has no submission_id: {page_key}")
        elif owner in owners:
            errors.append(f"duplicate submission owner: {owner}")
        else:
            owners.add(owner)
        if page_key in pages:
            errors.append(f"page submitted more than once in query: {page_key}")
        pages.add(page_key)
        if trace_outcome(row) not in TERMINAL_OUTCOMES:
            errors.append(f"submitted row has invalid terminal outcome: {owner}")

    summary = total.finish([str(args.trace)])
    for ok, message in ((summary["rows"] > 0, "trace contains no rows"),
                        (len(summary["run_ids"]) == 1, f"expected one run_id, found {summary['run_ids']}"),
                        (len(summary["policy_versions"]) == 1, f"expected one policy_version, found {summary['policy_versions']}"),
                        (len(summary["seeds"]) == 1, f"expected one seed, found {summary['seeds']}"),
                        (summary["useful_identity_ok"], "useful != ready_hit + pending_hit"),
                        (summary["terminal_identity_ok"], "submitted != useful + wasted")):
        if not ok:
            errors.append(message)
    if args.result_json:
        result = json.loads(args.result_json.read_text(encoding="utf-8"))
        limits = {"warmup_entry": 2, "pre_beam": 2, "rerank": 2,
                  "post_expand": int(result.get("prefetch_submit_top_k") or 4)}
        for group, count in selected.items():
            if trigger_by_group.get(group) in limits and count > limits[trigger_by_group[group]]:
                errors.append(f"trigger budget exceeded: group={group}")
        for query, groups in warmups.items():
            if len(groups) > 1:
                errors.append(f"entry warmup ran more than once: query={query} groups={len(groups)}")
        pool_limit = int(result.get("prefetch_candidate_pool_size") or 0) or int(result.get("effective_beam_width") or 0) * 4
        for group, group_pages in post_expand.items():
            group_pages.discard("")
            if pool_limit and len(group_pages) > pool_limit:
                errors.append(f"post_expand candidate pool exceeded: group={group}")
        stats = result.get("stats", {})
        for name in ("submitted", "ready_hit", "pending_hit", "useful", "wasted"):
            expected = stats.get("prefetch_" + ("useful_pages" if name == "useful" else "wasted_pages" if name == "wasted" else name))
            if expected is not None and summary[name] != expected:
                errors.append(f"trace/result mismatch for {name}: trace={summary[name]} result={expected}")
    payload = {"status": "ok" if not errors else "failed", "trace": str(args.trace),
               "summary": summary,
               "by_trigger": {key: value.finish([str(args.trace)]) for key, value in sorted(triggers.items())},
               "errors": errors}
    write_payload(payload, args.json_out)
    if errors:
        raise SystemExit(1)


def main() -> None:
    root = argparse.ArgumentParser()
    commands = root.add_subparsers(dest="command", required=True)
    analyze_parser = commands.add_parser("analyze")
    analyze_parser.add_argument("--trace", nargs="+", required=True, type=Path)
    analyze_parser.add_argument("--json-out", type=Path)
    analyze_parser.add_argument("--group-by", choices=("global", "query", "trigger"), default="global")
    analyze_parser.set_defaults(handler=analyze)
    validate_parser = commands.add_parser("validate")
    validate_parser.add_argument("--trace", required=True, type=Path)
    validate_parser.add_argument("--result-json", type=Path)
    validate_parser.add_argument("--json-out", type=Path)
    validate_parser.set_defaults(handler=validate)
    args = root.parse_args()
    args.handler(args)


if __name__ == "__main__":
    main()
