# Topology-Aware Async Prefetch

`agent-aware-search` exposes two online policies: `none` and `topology`. The default
remains `none` until the SIFT1M paired acceptance suite passes.

The topology policy runs after each beam batch is expanded. New neighbors and
the existing frontier are merged and sorted by PQ ADC distance, with node ID as
the deterministic tie breaker. At most `2 * beam_width` nodes are inspected.
Their packed pages are aggregated and ordered by best PQ rank, candidate count,
and page ID. This ranking only schedules I/O; it never changes the search heap
or exact rerank semantics.

`QueryPageSession` rejects pages already local, cached, ready, pending, or
previously submitted by the query. Demand reads reserve queue capacity and are
submitted before speculative work. Prefetch uses only a real io_uring backend;
`pread` and O_DIRECT fallback disable runtime prefetch instead of performing
synchronous speculative reads.

The adaptive controller starts with one page per window for 32 feedback
samples. It can raise the cap to two pages when ready hits are high and waste is
low, require more lead when pending hits dominate, or pause eight windows after
low usefulness or high waste. Query budget, inflight cap, io_depth, and reader
capacity further constrain submission.

Use:

```bash
./build/agent-aware-search \
  --enable-prefetch 1 \
  --prefetch-policy topology \
  --prefetch-adaptive 1 \
  --prefetch-top-k 2 \
  --prefetch-max-inflight 2 \
  --prefetch-trace logs/topology.csv
```

The JSON result reports runtime enablement, candidate/submission counts,
ready/pending/useful/wasted pages, demand-priority blocks, adaptive state,
planner CPU ratio, and I/O amplification. `scripts/agent_aware.py ab` runs
`no_prefetch`, `topology_fixed`, and `topology_adaptive` for cold and hot cache
conditions. `scripts/summarize_prefetch_ab.py` applies the five-round latency,
recall, read-amplification, usefulness, and CPU gates.
