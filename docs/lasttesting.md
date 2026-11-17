# 最近一次测试

全量四组都通过时，改写下面的「全量」，只留最近一次。写明日期、平台、线程模式、通过项数，以及机器的 CPU 型号和实测频率。只跑了部分用例不要写成全量通过。操作步骤仍在 [testing.md](testing.md)。

## 全量

2026-11-18，`197a4c3` 之后的 RPC 路由瘦身提交（RouteManager/ProtoMultiClientManager 收拢、Notify 补齐、去掉 RpcCall("名.区") 与 RpcCallBatch），提交前的工作区。四组都是 `--case all` 的 21 项，不含协程压力和序列化压力，也不含矩阵前置检查、MySQL、registry-static。用例：echo、log、log-backpressure、redis、redis-limit、redis-shutdown、redis-queue-timeout、rpc-ingress、redis-backpressure-bytes、redis-backpressure-count、redis-fairness、hold、rpc-admission-count、rpc-admission-bytes、reset、disk-full、registry、udp、multi、local、web。

| 平台 | 线程 | CPU | 频率（MHz） | 通过 | 收尾 |
| --- | --- | --- | --- | --- | --- |
| 本机 Windows | 多线程 | AMD Ryzen 7 9800X3D，8 核 16 线程，标称 4700 | 约 4419 | 21 | 约 366 秒。Visual Studio 17 2022 x64，`build-win`。回声 `ok=1000000 mismatch=0 rpc_fail=0`，`hold timer update tick=2 second=1 overlap=0/0 skipped=4 bad=0` |
| 本机 Windows | 单线程 | 同上 | 约 4419 | 21 | 约 436 秒，`build-win-st` 专用目录。`hold timer update tick=3 second=1 overlap=0/0 skipped=12 bad=0` |
| 本机 WSL | 多线程 | 同上 | 约 4419 | 21 | 约 298 秒，GCC 13。另跑 MySQL、registry-static 通过。`hold timer update tick=3 second=1 overlap=0/0 skipped=10 bad=0` |
| 本机 WSL | 单线程 | 同上 | 约 4419 | 21 | 约 269 秒，专用单线程目录，GCC 13。另跑 MySQL、registry-static 通过。`hold timer update tick=4 second=1 overlap=0/0 skipped=16 bad=0` |

四组并行跑。Windows 两路是 Visual Studio 17 2022 x64 Release；Linux 两路是本机 WSL 的 GCC 13 Release，不是远程机器，和 Windows 同一颗 CPU。表中秒数是各路 `parallel.json` 的整路耗时，含前置检查、配置、编译和用例。前置检查六项（field-names、inheritance-tests、inheritance、process-fixtures、test-safety、matrix-outcomes）四路退出码都是 0，不计入上面的 21。MySQL 与 registry-static 只在 WSL 两路另跑并通过，也不计入 21；MySQL 客户端日志为 `mysql done bad=0`。Windows 两路打开测试注入（`-DOI_ENABLE_TEST_HOOKS=ON`），WSL 两路关闭测试注入并打开 MySQL（`-DOI_ENABLE_MYSQL=ON`）。频率是全量结束后的空闲或轻载采样，不是跑测试当时的频率：`Get-Counter '\Processor Information(_Total)\% Processor Performance'` 五次为 94.36%、93.66%、94.28%、94.45%、93.41%，均值约 94.03%，乘 `Win32_Processor.MaxClockSpeed` 标称 4700，约 4419 MHz。`CurrentClockSpeed` 仍报 4700，没有拿它当实测频率。

## 显式用例

都是退出码 0。协程 `bad=0`。

机器：

| 机器 | CPU | 频率（MHz） |
| --- | --- | --- |
| 远程 4 核 | Intel Xeon Platinum 8255C，4 核 | 标称 2500，实测 2494 |
| 远程 2 核 | Intel Xeon Gold 6271C，2 核 | 标称 2600，实测约 2594 |
| 本机 | AMD Ryzen 7 9800X3D，8 核 16 线程 | 约 4700 |

### 回声

| 平台 | 线程 | 总量（次） | ok | mismatch | rpc_fail | 耗时（秒） | 速率（次/秒） |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 远程 4 核 | 多线程 | 1000000 | 1000000 | 0 | 0 | 12.9 | 77453 |

### 协程

三段端到端：`live`（`StartTask` 到全部挂起）、`wake`（`Schedule` 到跑完）、`churn`（空任务 `StartTask` 到跑完）。纳秒/条与条/秒由日志直接给出。挂起路径的 `StartTask→跑完` = `10^9 / (live + wake)`。本机四路与远程 4 核多线程、2 核单线程都已用新用例跑完；本机单线程测完已编回多线程，远程 2 核未编回。Windows 内存读 WorkingSet。

| 平台 | 线程 | 工人 | 每条内存（字节） | live（纳秒/条） | wake（纳秒/条） | 挂起路径 StartTask→跑完（条/秒） | churn（纳秒/条） | 空任务 StartTask→跑完（条/秒） |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 远程 4 核 | 多线程 | 3 | 201 | 2052 | 443 | 400801 | 1024 | 976562 |
| 远程 2 核 | 单线程 | 1 | 177 | 190 | 67 | 3891050 | 217 | 4608294 |
| 本机 WSL | 单线程 | 1 | 192 | 69 | 25 | 10638297 | 77 | 12987012 |
| 本机 WSL | 多线程 | 3 | 197 | 4339 | 135 | 223513 | 268 | 3731343 |
| 本机 Windows | 单线程 | 1 | 210 | 204 | 116 | 3125000 | 293 | 3412969 |
| 本机 Windows | 多线程 | 3 | 255 | 444 | 153 | 1675041 | 298 | 3355704 |

本机 WSL 上，多线程 `live` 比单线程大约慢 60 倍，`churn` 也差一截。Windows 多线程 `live` 也比单线程慢一截。批量 `StartTask` 抢共享队列锁，问题记在 [todo.md](../todo.md)「多线程协程批量创建/入队偏慢」，回头再优化。

### 序列化

1000 条协议，每条 128 个 `int32_t`。同一台远程 4 核。细节见 [字段分发](pb-dispatch.md)。

| 方案 | 编译（秒） | 解码（纳秒） |
| --- | --- | --- |
| 宏 | 298 | 2264 |
| switch | 1856 | 1282 |
