# 复现脚本

`docs/experiments/` 中的表格全部由这里的两组脚本产出。C++ 版本不依赖 Python，用于本机
（Windows / MSVC，无 cmake、无可用 Python）；`.py` 版本用于有 Python 的环境，二者矩阵与
表格口径必须保持一致。

> 生成 `results/2026-10-25-windows-msvc-v2/` 时本机装有 MSVC 14.42（VS 2022 Community）；
> 2026-10-25 复查时该安装已被移除，`C:\Program Files\Microsoft Visual Studio\2022` 为空目录，
> 本机仅剩 MinGW g++ 8.1。其 `<filesystem>` 实现有缺陷（`path::operator/=` 无法编译），
> 无法用于构建本工程。复跑这些脚本前需重新安装 MSVC（或改用有现代 GCC/Clang 的 Linux 环境）。

## 1. 合成数据集

```bat
cl /nologo /EHsc /O2 /MD /std:c++17 /utf-8 scripts\make_repro_dataset.cpp
make_repro_dataset.exe --out build\repro_gmm --base 100000 --queries 2000
```

输出 `sift_base.fvecs`、`sift_query.fvecs`、`sift_groundtruth.ivecs`（文件名与 SIFT1M 一致，
可直接喂给 `--sift-dir`）以及 `RECIPE.txt`。配方 `gmm-v1` 与种子 `20261025` 固定，字节级确定。

## 2. 索引

```bat
build\bin\agent-aware-build.exe --sift-dir build\repro_gmm --base-limit 100000 ^
  --pq-subspaces 32 --pq-centroids 256 --pq-train-limit 100000 --pq-iterations 8 ^
  --index-path build\i100.idx
```

100k × 128、graph degree 64、page 4096 的索引在本机约 7.5 分钟。

## 3. 运行矩阵

```bat
cl /nologo /EHsc /O2 /MD /std:c++17 /utf-8 scripts\run_repro_experiments.cpp
run_repro_experiments.exe --bin-dir build\bin --index build\i100.idx ^
  --dataset build\repro_gmm --out results\<run> --scratch build\repro_scratch ^
  --query-limit 2000 --mixed-duration 30
```

- 搜索矩阵：`none / lru / 2q / graph-aware-2q` × `96 / 128 / 156 / 192` × 3 轮，
  单线程、`beam_width=16`、`io-mode pread`、`memory-budget-ratio 0.20`。
- 混合读写：`read-only / 95-5-no-compaction / 95-5-compaction / 80-20-compaction` × 3 轮，
  每次 30 s、4 reader + 1 writer、CRUD 70/25/5、关闭 exact recall 采样。
- `failure/`：同一个动态目录先写 95/5、干净关闭，再只读重开，记录关闭与重开时的恢复报告。
- 每个配置使用全新的 `--scratch` 子目录；运行结束保留目录，便于事后检查 manifest/SSTable。

Windows 上矩阵由 `CreateProcess` 直接拉起 benchmark（不经 `cmd.exe`）：Git-bash 注入 PATH 的
MSYS 运行时会改写 `cmd.exe` 的命令行，导致参数中的路径被破坏。

## 4. 汇总

```bat
cl /nologo /EHsc /O2 /MD /std:c++17 /utf-8 scripts\summarize_repro_results.cpp
summarize_repro_results.exe results\<run>
```

输出搜索矩阵、混合读写、恢复检查三张 Markdown 表；每个单元格为 3 轮中位数，括号内为最小-最大。

实验驱动带 `--duration_sec` 运行时，`bench_mixed_rw` 把指标包在 `scenarios[0]` 里；两个版本的
`scenario_of` / `mixed_scenario` 都会把它提升到顶层。`results/2026-10-25-windows-msvc-v2/summary.md`
的混合读写表是修复该解析**之前**的 C++ 汇总产出的，整列为 `-`，应以
[`docs/experiments/mixed-read-write.md`](../docs/experiments/mixed-read-write.md) 的表为准
（两者数字一致，只是那张 summary 没读出来）。
