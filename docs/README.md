# 文档索引

本仓库的文档分**两代**存放，分界线是 2026-10-25 开始的重构（从"用调用次数冒充 I/O"
到 Phase 0/1 建立可信测量）。两代文档的代码基线、环境与数字口径都不同，**不能横向比较**。

| 代 | 目录 | 时间 | 代表代码 | 是否可引用 |
| --- | --- | --- | --- | --- |
| 重构前 | [`legacy/`](legacy/) | 2026-10-25 之前（2026-05 ~ 2026-07 撰写） | `6d7b69d` 及更早 | ❌ 只作历史归档 |
| 重构后 | [`experiments/`](experiments/)、[`problem/`](problem/) | 2026-10-25 之后 | `b9f3e46` → `783de3b` → `0a0a3ac` | ✅ 当前口径 |

---

## 1. 重构后（当前口径）

### 1.1 `experiments/` — 可复现实验报告

环境：**Windows MSVC、合成 100k×128（recipe `gmm-v1`）、`pread`、3 轮中位数**。
原始 JSON 在 [`results/2026-10-25-windows-msvc-v2/`](../results/2026-10-25-windows-msvc-v2/)。

| 文档 | 内容 |
| --- | --- |
| [`README.md`](experiments/README.md) | 总索引、阅读顺序、与旧记录的差异 |
| [`reproducibility.md`](experiments/reproducibility.md) | 环境、数据集配方、矩阵、字段口径、已知限制 |
| [`search-width-tuning.md`](experiments/search-width-tuning.md) | search width × cache policy 的 Recall/延迟/I/O 对照 |
| [`cache-policy.md`](experiments/cache-policy.md) | `none / lru / 2q / graph-aware-2q` 对照 |
| [`mixed-read-write.md`](experiments/mixed-read-write.md) | 纯读 / 95-5 / 80-20 × compaction 开关、恢复检查 |
| [`memory-budget.md`](experiments/memory-budget.md) | 模型估算 vs 真实 RSS，预算是否满足 |

### 1.2 `problem/` — 问题审计、修复与分阶段测量

环境：**VMware Ubuntu 26.04 / 内核 7.0 / GCC 15.2 / 8 vCPU / 12 GB，ext4 原生盘
（`/dev/sdb`，仓库 `/data/agent-aware`），SIFT1M，`vmtouch -e` 真冷缓存**。
Windows 侧通过 [`scripts/sync-repo-to-vm.sh`](../scripts/sync-repo-to-vm.sh) 同步到 VM。

| 文档 | 内容 |
| --- | --- |
| [`current-issues.md`](problem/current-issues.md) | 2026-10-25 审计出的全部问题（文档漂移、指标失真、死代码等） |
| [`fix-status-2026-10-25.md`](problem/fix-status-2026-10-25.md) | 上表逐条修复状态（已修复 / 部分修复 / 未修复） |
| [`retrospective-2026-10-25.md`](problem/retrospective-2026-10-25.md) | 复盘：错误机制与教训（"用调用次数冒充 I/O"等） |
| [`phase0-measurements-2026-10-26.md`](problem/phase0-measurements-2026-10-26.md) | Phase 0：物理 I/O / 缓存状态 / RSS 三条硬前提，首次可信测量 |
| [`phase1-demand-io-batching-2026-10-27.md`](problem/phase1-demand-io-batching-2026-10-27.md) | Phase 1：demand 读批处理与分阶段 I/O 归因，syscall −91%、延迟 −55% |

**Phase 0/1 的数字全部来自 VM**（`phase0` 头部写明 `/data/agent-aware`，`phase1` 写明
`/data/agent-aware-new` 与 `/data/work`）。跨日、跨二进制状态的数字不能直接比，
`phase1` 只做同机同日交替对照。

---

## 2. 重构前（历史归档，`legacy/`）

> ⚠️ 这些文档里的接口名、参数默认值、路径和数字**不代表当前实现**。评审材料中
> 如需引用，先对照 [`problem/current-issues.md`](problem/current-issues.md) 的审计结论。

| 路径 | 内容 |
| --- | --- |
| [`legacy/design/`](legacy/design/) | 01-06 编号模块设计稿：SSD 存储链路、缓存零拷贝、异步预取、beam_width/io_uring、动态写入、streaming ANN |
| [`legacy/roadmap/`](legacy/roadmap/) | 原 `roadmap in future/`：CRTP 两阶段预取、答辩缺口补强、SIFT1M 混合读写优化、Recall→尾延迟优化（约 3600 行，未清理优先级） |
| [`legacy/problem.md`](legacy/problem.md) | 赛题背景与问题定义（与根目录 `赛题.md` 逐字节相同） |
| [`legacy/AI_USAGE_DISCLOSURE.md`](legacy/AI_USAGE_DISCLOSURE.md) | AI 工具使用声明（答辩材料仍需引用） |
| [`legacy/archive/2026-10-legacy/`](legacy/archive/2026-10-legacy/) | 旧 SIFT1M 实验报告 + `8670a61` 的源码快照 `legacy-source/`，说明为什么废弃 |

`legacy/design/04` 与 `legacy/design/05` 里的相对链接已指向
[`experiments/`](experiments/)（重构后文档仍在原位置）。

---

## 3. 根目录文档

| 文档 | 定位 |
| --- | --- |
| [`../README.md`](../README.md) | 项目主 README：架构、构建、命令、核心特性与当前结论 |
| [`../PROJECT_PLAN.md`](../PROJECT_PLAN.md) | 项目计划书：目标、里程碑、指标映射、文档指引（§16） |
| [`../results/README.md`](../results/README.md) | 原始 JSON 目录说明与数据集配方 |
| [`../scripts/README.md`](../scripts/README.md) | 数据集生成、矩阵运行、汇总脚本用法 |

---

## 4. 新增文档时放哪里

- 能由当前代码直接产出、有原始 JSON 的实验 → `experiments/`，并在 `results/` 留 JSON；
- 问题审计、修复记录、按阶段（Phase N）的测量与复盘 → `problem/`，文件名带日期；
- 已不代表当前实现的旧稿 → `legacy/`，并在文档头标注"历史归档"；
- 不要新建"未来计划"类目录：路线图类内容统一进 `legacy/roadmap/`，或直接写成
  `problem/` 下的带日期的待办。
