# 重构前文档（历史归档）

> **这里的任何内容都不代表当前实现。** 接口名、参数默认值、目录路径与数字均停留在
> 2026-10-25 重构之前，只用于追溯"当时是怎么想的"。

分界点是 2026-10-25 开始的重构：此前仓库的 I/O 数字全是 `pread` 调用计数，
而数据集完全驻留在页缓存里，等于在测内存延迟。重构后建立了物理 I/O、缓存状态、
峰值 RSS 三条可验证前提，过程见 [`../problem/retrospective-2026-10-25.md`](../problem/retrospective-2026-10-25.md)
与 [`../problem/phase0-measurements-2026-10-26.md`](../problem/phase0-measurements-2026-10-26.md)。

| 子目录 | 内容 | 撰写时间 |
| --- | --- | --- |
| [`design/`](design/) | 01-06 模块设计稿：SSD 存储链路、缓存零拷贝、异步预取、beam_width/io_uring、动态写入、streaming ANN | 2026-06 ~ 2026-07 |
| [`roadmap/`](roadmap/) | 原 `roadmap in future/`：CRTP 两阶段预取、答辩缺口补强、SIFT1M 混合读写优化、Recall→尾延迟优化 | 2026-06 ~ 2026-07 |
| [`problem.md`](problem.md) | 赛题背景与问题定义（与根目录 `赛题.md` 逐字节相同） | 2026-06 |
| [`AI_USAGE_DISCLOSURE.md`](AI_USAGE_DISCLOSURE.md) | AI 工具使用声明，答辩材料仍需引用 | 2026-06 |
| [`archive/2026-10-legacy/`](archive/2026-10-legacy/) | 旧 SIFT1M 实验报告与 `8670a61` 源码快照，附"为什么废弃"的逐条证据 | 2026-07 |

## 引用规则

1. 评审/答辩中如需提到这些文档，先对照 [`../problem/current-issues.md`](../problem/current-issues.md)
   的审计结论（那里逐条列出了哪些设计没有落地、哪些路径已删除、哪些表格是空的）。
2. 数字一律不用。当前可信口径只有两套：`../problem/phase0|phase1-*.md`（SIFT1M / VM / 冷缓存）
   与 `../experiments/`（合成 100k×128 / Windows / `pread`），两者也不能横向比较。
3. `design/04`、`design/05` 里的相对链接已指向 [`../experiments/`](../experiments/)；
   `roadmap/` 内部指向已删除文件的链接保持原样，作为"当时计划与后来实现脱节"的证据。

完整的文档索引见 [`../README.md`](../README.md)。
