# WSL / Linux 严格 Cold Start 验证方法

V2 当前归档结果使用的是 synthetic clustered workload，不是 SIFT 数据集：

- dataset type: synthetic
- base_count: 2000
- query_count: 200
- dim: 64
- clusters: 32
- seed: 42
- ground truth: V0 exact brute force

当前 Windows 归档中的 `cold` 只能解释为 cold-like run，因为 Windows 开发环境没有可靠清空系统 Page Cache 的流程。要做更规范的 cold start，应在 WSL 或 Linux 中运行。

## 推荐运行位置

可以在 WSL 中访问 `/mnt/c/...` 下的仓库，但严格 I/O 实验更推荐把仓库和 index 文件放在 WSL ext4 文件系统中，例如：

```bash
~/agentmem-flow
```

原因是 `/mnt/c` 经过 Windows 文件系统转接层，I/O 行为和 Linux 原生 ext4 不完全一致。`drop_caches` 可以清 Linux Page Cache，但不能让 `/mnt/c` 的路径完全等价于原生 Linux 块设备实验。

## 构建

```bash
bash scripts/linux/build.sh
```

## 严格 cold / warm V2 对比

```bash
bash scripts/linux/run_v2_strict_cold_warm.sh
```

脚本会执行：

1. 构建 one-node index。
2. 构建 coaccess packed index。
3. 每个 cold run 前执行：

```bash
sync
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
```

4. 分别记录 one-node cold、coaccess cold、one-node warm、coaccess warm。

## 权限要求

严格 cold start 需要写 `/proc/sys/vm/drop_caches`，因此需要 root 或 sudo 权限。如果没有 sudo 权限，只能做 warm/smoke 或 cold-like 实验，不能宣称为严格 cold run。

## 归档要求

正式 WSL/Linux cold run 需要归档：

```text
archive/results/v2-linux-strict-cold-*.txt
archive/configs/v2-linux-strict-cold-*.json
archive/logs/v2-linux-strict-cold-*.log
archive/build_info/v2-linux-strict-cold-*.txt
```

build info 中必须额外记录：

- WSL 版本或 Linux 发行版。
- 文件路径是否位于 WSL ext4。
- `drop_caches` 是否成功。
- `mount` / filesystem 信息。
- 是否使用 sudo。

