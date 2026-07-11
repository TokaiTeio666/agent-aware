# MemHarmony 十天落地计划

## 目标

3 人并行，在第 10 天交付可安装的 HarmonyOS NEXT/API 12+ 真机版本，以及初赛所需源码、技术方案、测试报告和演示材料。

作品定位为“个人知识记忆系统”：全程离线完成公开校园知识导入、端侧向量化、语义检索、记忆新增/更新/删除、重启恢复和资源自适应调度。

明确不做：多设备同步、云端向量化、真机实时建大图、系统权限修改。XGBoost 预取暂不作为主要性能结论，除非真机 A/B 数据证明收益。

## 技术方案

- ArkTS Stage 应用负责搜索、记忆管理、RDB 文本元数据、诊断面板和演示数据重置。
- NAPI/C++ 层复用现有 `StorageEngine`，负责文本向量化、PQ/Vamana 检索、Graph-Aware 2Q、WAL/MemTable/SSTable 和指标采集。
- 主模型采用 `BAAI/bge-small-zh-v1.5`：512 维、INT8、最大 256 tokens、CLS pooling、L2 归一化，通过 MindSpore Lite Native 推理。
- 第 2 天 12:00 设置模型闸门：MindSpore Lite 无法稳定运行时，切换到 NCNN 后端；上层接口、模型维度和数据格式不变。
- 预置 5,000 条公开校园知识切片，每条 200-400 字，离线生成 `base.idx + base.pq + base_meta.json`；用户新增记录使用 ID `>=5000` 并进入动态层。
- 增加 PQ 模型版本化持久化格式，包含 magic、版本、维度、子空间、码本、codes 和校验和；初始化时拒绝模型、索引维度不一致。
- 为 OpenHarmony 增加 `thread_pool + pread` 异步后端：有界队列、查询读优先于预取、深度默认 8；不依赖 `io_uring/O_DIRECT`。
- ARM64 增加 NEON L2 距离实现，保留标量路径用于一致性校验。
- 自适应资源档位固定为：`NORMAL` 使用 20% 向量数据大小作为缓存、深度 8、预取 4 页；`CONSTRAINED` 为 10%/4/2；`CRITICAL` 为 4%/1/关闭预取。内存事件仅在查询间切换档位，避免并发重建缓存。

### NAPI 接口

所有耗时接口返回 `Promise`，不得阻塞 UI 线程：

```ts
initialize(config): Promise<EngineInfo>
embed(text: string): Promise<Float32Array>
searchVector(vector: Float32Array, topK: number): Promise<SearchHit[]>
upsertVector(id: number, vector: Float32Array): Promise<void>
deleteVector(id: number): Promise<void>
setResourceProfile(profile: "NORMAL" | "CONSTRAINED" | "CRITICAL"): Promise<void>
getMetrics(): Promise<EngineMetrics>
shutdown(): Promise<void>
```

ArkTS `MemoryService` 组合向量接口与 RDB。新增时先写 `PENDING` 元数据，再写 Native WAL，成功后标记 `ACTIVE`；失败则回滚。启动时重新处理 `PENDING`，删除记录使用 tombstone，确保重启后不会重新出现。

## 十天排期

| 天 | Native 工程师 A | 鸿蒙应用工程师 B | 数据/测试/材料工程师 C | 当日验收 |
|---|---|---|---|---|
| D1 | 建立 OHOS Native CMake，裁剪 benchmark | 创建 Stage 工程、签名、NAPI Hello | 确认设备/API、模型和数据许可证 | 真机安装 HAP，ArkTS 成功调用 C++ |
| D2 | 端侧模型、tokenizer、PQ save/load | 实现异步 Native 调用封装 | 制作 20 条端到端验证数据 | 真机完成文本→向量→Top-K；中午执行模型闸门 |
| D3 | 适配 `__OHOS__`、pread、文件沙箱路径 | 首页搜索、结果列表、记录详情 | 清洗并切分 5,000 条校园知识，离线建索引 | 应用能查询预置 base，结果文本映射正确 |
| D4 | 接通 DynamicWriteManager 与恢复逻辑 | 新增、编辑、删除及 RDB 状态机 | 编写 CRUD/崩溃恢复用例 | 四项操作和杀进程重启恢复全部通过 |
| D5 | 实现 thread-pool I/O、读优先和 NEON | 接入资源档位、内存回调 | 构造 baseline/adaptive 测试脚本 | 真机显示有效 I/O 后端、缓存和预取统计 |
| D6 | Graph-Aware 2Q 参数与并发稳定性 | 完成诊断页、模式切换、导入和重置 | 建立 50 条人工语义查询集 | 演示闭环完整，连续 200 次查询无崩溃 |
| D7 | 修复性能热点和生命周期问题 | 完成交互、空态、错误态和离线提示 | 冷/热缓存各跑 5 轮 A/B | 生成可追溯 JSON/CSV，不使用单次最好结果 |
| D8 | 冻结核心代码、执行 ASan/主机回归 | 生成 release HAP 并验证升级/重装 | 完成正确性、性能、内存和恢复测试报告 | Recall、延迟、RSS、I/O、写入数据齐全 |
| D9 | 整理源码结构和第三方许可 | 录制 3 分钟演示素材 | 完成方案书、架构图、PPT、AI 使用声明 | 所有材料中的指标均能追溯到结果文件 |
| D10 | 全新环境构建复核 | 真机最终演练、HAP 校验 | 答辩问答、提交清单、压缩包校验 | 一键构建成功，完整演示两遍无故障 |

人员安排固定：A 只负责 Native/性能主线，B 只负责 ArkTS/真机集成，C 负责数据、自动化测试、报告和视频；每天结束进行一次主分支集成，不在最后三天合并大功能。

## 测试与验收

- 功能：预置检索、新增、更新、删除、重复 ID、空文本、超长文本、损坏资源、杀进程恢复、离线运行。
- 正确性：ANN Recall@10 相对全量精确检索 `>=0.90`；50 条人工查询 Top-3 命中率 `>=80%`；Native 与标量距离误差 `<1e-4`。
- 性能：200 条固定查询，冷/热缓存各 5 轮，报告中位数与 P95/P99；端到端查询 P95 目标 `<800ms`，纯向量检索 P95 目标 `<250ms`。
- 稳定性：连续 500 次查询、100 次混合 CRUD、10 次强制重启，零 Native 崩溃、零已确认记录丢失。
- 资源：记录峰值 RSS、模型加载时间、缓存占比和包体；`CRITICAL` 档位必须关闭预取并成功完成查询。
- 对照组：同步 pread 无预取、固定缓存异步预取、自适应调度三组。只有多轮中位数改善 `>=10%` 或 demand reads 下降 `>=20%` 时，材料才使用“显著提升”表述。
- 提交完整性：源码、构建说明、签名 HAP、模型/数据许可证、技术方案、原始结果、测试报告、演示视频、既往参赛及 AI 使用声明全部存在且路径一致。

## 假设

- 使用单台 HarmonyOS NEXT/API 12+ ARM64 真机和新版 DevEco Studio Native SDK。
- 真机允许安装调试签名 HAP，应用可访问自身沙箱文件。
- 初赛版本不访问网络、不读取用户真实隐私数据。
- 第 10 天完成内部候选包，保留正式截止日前的余量处理签名、材料格式和组委会反馈。