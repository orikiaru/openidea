# 最近一次测试

全量四组都通过时，改写下面的「全量」，只留最近一次。写明日期、平台、线程模式、通过项数，以及机器的 CPU 型号和实测频率。只跑了部分用例不要写成全量通过。操作步骤仍在 [testing.md](testing.md)。

## 全量

2026-11-14。四组都是 `--case all` 的 20 项，不含协程压力和序列化压力。用例：echo、log、log-backpressure、redis、redis-limit、redis-shutdown、redis-queue-timeout、rpc-ingress、redis-backpressure-bytes、redis-backpressure-count、redis-fairness、hold、rpc-admission-count、rpc-admission-bytes、reset、disk-full、registry、udp、multi、web。

| 平台 | 线程 | CPU | 频率（MHz） | 通过 | 收尾 |
| --- | --- | --- | --- | --- | --- |
| 本机 Windows | 多线程 | AMD Ryzen 7 9800X3D，8 核 16 线程 | 约 4639 | 20 | 已编回多线程，约 345 秒。`hold timer update tick=3 second=1 overlap=0/0 skipped=10 bad=0` |
| 本机 Windows | 单线程 | 同上 | 约 4639 | 20 | 已编回多线程，约 398 秒。`hold timer update tick=3 second=1 overlap=0/0 skipped=10 bad=0` |
| 远程 Linux 4 核 | 多线程 | Intel Xeon Platinum 8255C | 2494 | 20 | 已编回多线程。`hold timer update tick=4 second=1 overlap=0/0 skipped=16 bad=0` |
| 远程 Linux 2 核 | 单线程 | Intel Xeon Gold 6271C | 约 2594（历史实测） | 20 | 专用单线程机器，按 `--no-restore` 未编回多线程；测试后 SSH 超时，最终清理状态未能确认。 |

四组并行跑。Windows 频率按测试后的 `% Processor Performance` 三次采样均值（约 98.7%）乘标称 4700 估算；远程单线程机器的频率沿用下方历史实测，本次因 SSH 超时未能复测。首次 Windows 两路因进程正常退出超时失败，改用新控制台重跑后通过。

四组完成后为消除 MSVC 告警修改了 Windows 日志测试钩子，补跑 Windows 多线程 `log`、`log-backpressure`、`disk-full` 均通过；日志服固定多线程。

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
