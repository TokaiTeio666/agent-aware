# SIFT Experiment Report

## Scope

This report now tracks only the SIFT validation results that remain part of the
current formal evidence set. Local synthetic runs and Windows local smoke runs
have been removed from this document.

## Active Validation Set

- Dataset: `SIFT10K`
- Dataset: `SIFT100K`
- Environment: WSL2 Ubuntu
- Path: `/mnt/d/agent-aware`
- I/O backend: `pread`
- Memory budget: `--memory-budget-ratio 0.20 --enforce-memory-budget`

## Verified Results

| Dataset | Profile | Recall@10 | QPS | P99 ms | SSD reads/query | Resident/raw | Budget pass |
|---|---|---:|---:|---:|---:|---:|---|
| SIFT10K | lsh-rp BFS, early-stop min 192 | 0.9990 | 3.8299 | 731.7332 | 561.7500 | 11.42% | yes |
| SIFT100K | lsh-rp BFS, early-stop min 192 | 0.9640 | 2.4614 | 1678.1738 | 1934.1100 | 9.06% | yes |
| SIFT10K | PQ4x16 + ADC + rerank64 | 0.4460 | 4.6187 | 648.3881 | 594.5700 | 12.86% | yes |
| SIFT10K | lsh-rp BFS + cache near 20% | 0.9990 | 8.6361 | 278.0768 | 550.7600 | 19.10% | yes |
| SIFT100K | lsh-rp BFS + cache near 20% | 0.9640 | 1.9070 | 2667.5456 | 1855.4200 | 19.06% | yes |

## Findings

- The current formal validation path is the no-cache lsh-rp graph profile on
  `SIFT10K` and `SIFT100K`.
- `SIFT100K Recall@10=0.9640` is the current mainline result.
- Near-20% cache use is a budget-allocation comparison, not a new default.
- The current PQ/ADC profile remains a counterexample because recall is too low.

## I/O Mode Status

| Requested mode | Effective mode | Status |
|---|---|---|
| pread | pread | active |
| odirect | pread | native backend not implemented |
| io_uring | pread | native backend not implemented |

Current evidence does not support claiming `io_uring_enabled=1`. The repo still
falls back to `pread` for real execution.

## Evidence Files

- `archive/results/wsl-sift10k-lsh-budget-20260605-130016.txt`
- `archive/results/wsl-sift100k-lsh-budget-20260605-130120.txt`
- `archive/results/wsl-sift10k-lsh-cache20-20260605-131427.txt`
- `archive/results/wsl-sift100k-lsh-cache20-20260605-131500.txt`
- `archive/results/wsl-sift10k-m2-pq-budget-20260605-125914.txt`
- `archive/logs/wsl-sift10k-lsh-budget-20260605-130016.log`
- `archive/logs/wsl-sift100k-lsh-budget-20260605-130120.log`
- `archive/logs/wsl-sift10k-lsh-cache20-20260605-131427.log`
- `archive/logs/wsl-sift100k-lsh-cache20-20260605-131500.log`
- `archive/logs/wsl-sift10k-m2-pq-budget-20260605-125914.log`
