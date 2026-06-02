# V5.2 Analysis - Delta IVF-Flat

## Goal

V5.2 reduces query cost for larger delta segments while retaining
`DeltaFlatIndex` as the correctness reference.

## Implementation

- Added `DeltaIvfFlatIndex`, a small online IVF-flat delta ANN baseline with
  farthest-point centroid initialization and periodic k-means rebuilds.
- Added `--delta-index-policy flat|ivf-flat`.
- Added `--delta-ivf-centroids`, `--delta-ivf-probes`,
  `--delta-ivf-train-iterations`, and `--delta-ivf-rebuild-interval`.
- Mixed workload now records selected delta search latency, exact delta latency,
  and delta recall against flat delta.
- Added smoke script: `scripts/run_v5_2_delta_ann_compare.ps1`.

## Validation

Command:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\run_v5_2_delta_ann_compare.ps1
```

Observed comparison on 300 inserted delta vectors:

| Variant | Delta Search Avg | Exact Delta Avg | Delta Recall@10 | Overall Recall@10 |
|---|---:|---:|---:|---:|
| flat | 0.0046 ms | 0.0046 ms | 1.0000 | 0.9987 |
| ivf-flat | 0.0034 ms | 0.0047 ms | 0.9987 | 0.9987 |

## Status

Pass as a small ANN baseline. Periodic k-means rebuilds improved average delta
recall to 0.9987 while keeping selected delta search faster than flat on the
smoke workload. Insert latency is higher because rebuild work happens on the
write path, so larger final experiments should tune rebuild interval or move
training to a background maintenance path.
