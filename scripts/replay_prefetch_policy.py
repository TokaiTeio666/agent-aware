#!/usr/bin/env python3
import argparse
import csv
import json
from collections import defaultdict
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description="Replay topology page ranking against an oracle")
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--top-k", type=int, default=2)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    groups = defaultdict(list)
    with args.trace.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            groups[(row["run_id"], row["query_id"])].append(row)
    topology_hits = oracle_hits = selected = 0
    for rows in groups.values():
        ordered = sorted(rows, key=lambda row: (int(row["min_pq_rank"]),
                                                 -int(row["candidate_count"]),
                                                 int(row["page_id"])))
        topology_hits += sum(int(row["useful"]) for row in ordered[:args.top_k])
        useful = [row for row in rows if int(row["useful"])]
        oracle_hits += min(args.top_k, len(useful))
        selected += min(args.top_k, len(ordered))
    payload = {
        "policy": "topology", "queries": len(groups), "top_k": args.top_k,
        "selected": selected, "topology_useful": topology_hits,
        "oracle_useful": oracle_hits,
        "topology_precision": topology_hits / selected if selected else 0.0,
        "oracle_gap": oracle_hits - topology_hits,
    }
    text = json.dumps(payload, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    print(text, end="")


if __name__ == "__main__":
    main()
