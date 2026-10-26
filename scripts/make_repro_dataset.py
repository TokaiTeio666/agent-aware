#!/usr/bin/env python3
"""Generate a deterministic fvecs dataset for reproducible experiments.

The byte layout matches SIFT1M (``<int32 dim><dim * float32>`` records), so the
benchmarks read it exactly like ``data/sift/*.fvecs``. The generator is a PCG64
stream split into independent child streams for base/queries/truth, therefore
the output bytes depend only on (count, dim, seed, recipe version).

Recipe v1 (mixture of Gaussians, unit-ish scale):
  * centroids are drawn once per dataset (per child stream) from U(-1, 1)^dim;
  * every vector picks a centroid uniformly and adds N(0, 0.15^2) noise;
  * queries reuse the same centroids with N(0, 0.10^2) noise, so they sit
    inside the data manifold rather than in empty space;
  * truth = exact top-k by L2, computed in float64 over the sampled base.

Usage:
  python scripts/make_repro_dataset.py --out-dir build/repro_sift_like \\
      --base 100000 --queries 2000 --dim 128 --seed 20261025
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

import numpy as np

RECIPE = "gmm-v1"


def _write_fvecs(path: Path, vectors: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    dim = vectors.shape[1]
    record = struct.Struct(f"<i{dim}f")
    with path.open("wb") as stream:
        for row in vectors.astype(np.float32):
            stream.write(record.pack(dim, *row.tolist()))


def _centroids(rng: np.random.Generator, dim: int, clusters: int) -> np.ndarray:
    return rng.uniform(-1.0, 1.0, size=(clusters, dim)).astype(np.float32)


def _sample(rng: np.random.Generator, centroids: np.ndarray,
            count: int, dim: int, sigma: float) -> np.ndarray:
    idx = rng.integers(0, centroids.shape[0], size=count)
    noise = rng.normal(0.0, sigma, size=(count, dim)).astype(np.float32)
    return centroids[idx] + noise


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--base", type=int, default=100_000)
    parser.add_argument("--queries", type=int, default=2_000)
    parser.add_argument("--dim", type=int, default=128)
    parser.add_argument("--clusters", type=int, default=1_000)
    parser.add_argument("--seed", type=int, default=20261025)
    parser.add_argument("--top-k", type=int, default=10)
    args = parser.parse_args()

    out = Path(args.out_dir)
    base_rng, query_rng = (
        np.random.default_rng(child)
        for child in np.random.SeedSequence(args.seed).spawn(2)
    )
    centroids = _centroids(base_rng, args.dim, args.clusters)
    base = _sample(base_rng, centroids, args.base, args.dim, 0.15)
    queries = _sample(query_rng, centroids, args.queries, args.dim, 0.10)

    _write_fvecs(out / "sift_base.fvecs", base)
    _write_fvecs(out / "sift_query.fvecs", queries)

    # Exact truth, in chunks to bound peak memory for 1M-scale bases.
    truth = np.empty((args.queries, args.top_k), dtype=np.int32)
    base64 = base.astype(np.float64)
    base_norm = np.einsum("ij,ij->i", base64, base64)
    for start in range(0, args.queries, 256):
        chunk = queries[start:start + 256].astype(np.float64)
        dist = (np.einsum("ij,ij->i", chunk, chunk)[:, None] - 2.0 * chunk @ base64.T
                + base_norm[None, :])
        truth[start:start + chunk.shape[0]] = np.argsort(
            dist, axis=1, kind="stable")[:, :args.top_k]
    # .ivecs 格式：每个向量前有一个 int32 维度头，与 .fvecs 的 float32 维度头对应。
    # 之前直接写裸 int32 数据，C++ 读取端按 ivecs 解析会报 "truncated ivecs payload"。
    truth_ivecs = np.empty((args.queries, args.top_k + 1), dtype=np.int32)
    truth_ivecs[:, 0] = args.top_k
    truth_ivecs[:, 1:] = truth
    (out / "sift_groundtruth.ivecs").write_bytes(truth_ivecs.astype("<i4").tobytes())

    (out / "RECIPE.txt").write_text(
        f"recipe={RECIPE}\nbase={args.base}\nqueries={args.queries}\n"
        f"dim={args.dim}\nclusters={args.clusters}\nseed={args.seed}\n"
        f"top_k={args.top_k}\nsigma_base=0.15\nsigma_query=0.10\n",
        encoding="ascii",
    )
    print(f"wrote {out} ({RECIPE})")


if __name__ == "__main__":
    main()
