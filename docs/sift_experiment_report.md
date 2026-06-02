# SIFT Graph Search Experiment Report

## Scope

This report tracks reproducible SIFT experiments for `agent-aware`. Metrics are
only recorded when they come from real `key=value` result files in
`archive/results/` or real external-system logs.

## Environment

- Local validation host: Windows, MinGW GCC 8.1.0.
- Target experiment scripts: Linux Bash.
- Local Linux limitation: no WSL distribution is installed on this host.
- Dataset files are present under `data/sift/`, including the SIFT1M base,
  query, learn, and official ground-truth files.

## Implemented Interfaces

- Unified local runner: `scripts/linux/run_sift_local.sh`
- Matrix sweep: `scripts/linux/run_sift_sweep.sh`
- SIFT1M cold/warm runner: `scripts/linux/run_sift1m_full.sh`
- Feature comparison runner: `scripts/linux/run_sift_feature_compare.sh`
- CSV summarizer: `scripts/linux/summarize_sift_results.py`
- Comparison CSV summarizer: `scripts/linux/summarize_system_comparison.py`
- External wrappers: `scripts/linux/build_diskann.sh`,
  `scripts/linux/run_diskann_sift.sh`, and
  `scripts/linux/run_freshdiskann_sift.sh`

## Run Commands

```bash
# Small graph smoke
BASE_LIMIT=10000 QUERY_LIMIT=100 ENGINE=graph \
SEARCH_WIDTH=128 SEARCH_EARLY_STOP_MIN=64 \
bash scripts/linux/run_sift_local.sh

# Parameter sweep and CSV summary
bash scripts/linux/run_sift_sweep.sh
python3 scripts/linux/summarize_sift_results.py

# Hotpath, adaptive stop, and PQ/ADC comparison
bash scripts/linux/run_sift_feature_compare.sh

# SIFT1M cold/warm matrix
DROP_OS_CACHE=1 bash scripts/linux/run_sift1m_full.sh

# Regenerate same-machine comparison CSV
python3 scripts/linux/summarize_system_comparison.py
```

## Verified Local Smoke Results

The following SIFT10K, 100-query runs were executed locally on Windows with
real result files. They validate the interfaces, not a final performance claim.
Some feature smoke runs overlapped in time and should be rerun sequentially on
the Linux experiment host before comparing latency.

| Profile | Recall@10 | QPS | P99 ms | SSD reads/query | Notes |
|---|---:|---:|---:|---:|---|
| raw BFS packing | 0.9980 | 307.0676 | 5.3552 | 378.7400 | baseline interface smoke |
| hotpath packing | 0.9980 | 65.8301 | 30.1505 | 505.8900 | initial layout did not improve this smoke |
| adaptive early stop | 0.9980 | 92.8972 | 19.1791 | 379.6900 | expanded avg 64.6700 |
| PQ8 + ADC | 0.5760 | 291.9099 | 7.4577 | 388.4400 | approximate ranking loses substantial recall |
| PQ8 + ADC + rerank64 | 0.9620 | 389.6484 | 4.7913 | 388.7700 | PQ codes use 80,000 bytes vs 5,120,000 raw bytes |

## Verified SIFT100K Results

These Windows runs used the same packed BFS index and were executed
sequentially. The Linux sweep remains the final reporting target.

| Profile | Recall@10 | QPS | P99 ms | SSD reads/query |
|---|---:|---:|---:|---:|
| exact | 1.0000 | 164.2455 | 9.6533 | n/a |
| graph 128/64 | 0.8563 | 108.3395 | 17.0001 | 1132.5090 |
| graph 256/96 | 0.9085 | 86.7212 | 21.8221 | 1404.6090 |
| graph 512/128 | 0.9404 | 74.3867 | 24.2162 | 1630.9750 |
| graph 768/160 | 0.9553 | 70.9621 | 27.0958 | 1826.7430 |
| graph 1024/192 | 0.9651 | 31.5805 | 84.8826 | 1998.5390 |

The current SIFT100K recommendation under the required Recall@10 >= 0.95 rule
is `SEARCH_WIDTH=768`, `SEARCH_EARLY_STOP_MIN=160`. It has the lower P99 and
SSD reads/query of the two verified qualifying profiles.

## Current Findings

- PQ8 codes reduce resident vector-code storage by 64x in the verified smoke.
- PQ + ADC without rerank reduced Recall@10 to 0.5760. Reranking 64 raw-vector
  candidates recovered Recall@10 to 0.9620 in the verified sequential smoke.
- Initial hotpath packing did not improve the verified smoke. A likely reason
  is that sorting individually hot nodes is not the same as optimizing page
  co-occurrence. The current six nodes per page also limits local gains.
- Adaptive stop is functional and reports expansion percentiles. The verified
  smoke reached a similar read count to the raw baseline.

## Cold And Warm Status

`scripts/linux/run_sift1m_full.sh` runs exact and graph profiles in cold and
warm modes. It supports `DROP_OS_CACHE=1` when passwordless `sudo` is
available. No SIFT1M cold/warm numbers are reported yet because this local
Windows host has no WSL distribution.

## I/O Mode Status

| Requested mode | Effective mode | Native backend | Verification |
|---|---|---|---|
| pread | pread | available | baseline smoke passed |
| odirect | pread | TODO | fallback smoke passed and reported `io_direct_enabled=0` |
| io_uring | pread | TODO | fallback smoke passed and reported `io_uring_enabled=0` |

## External System Status

DiskANN and FreshDiskANN source trees and binaries are not present. The
wrappers emit an explicit failure status when their run commands are missing.
`archive/results/system_comparison.csv` is generated only from real logs.

## Pending Full Experiments

- SIFT10K and SIFT100K sequential sweeps still need to run on Linux.
- SIFT1M cold/warm experiments are scripted but have not been executed.
- `IO_MODE=odirect` and `IO_MODE=io_uring` currently accept configuration,
  compile, report explicit fallback fields, and run through `pread`. Native
  aligned `O_DIRECT` buffers and an `io_uring` submission queue remain TODO.
- DiskANN and FreshDiskANN wrappers are present, but no external checkout or
  binary is available in this workspace. No external comparison metrics have
  been fabricated.

## Next Measurements

1. Run `bash scripts/linux/run_sift_sweep.sh`.
2. Run `IO_MODE=odirect bash scripts/linux/run_sift1m_full.sh` after native
   O_DIRECT support is implemented, or use `DROP_OS_CACHE=1` with appropriate
   privileges for cold-cache experiments.
3. Place external checkouts under `third_party/`, build them, and provide the
   wrapper run commands for same-machine comparison.
