# 最近一次测试

全量四组都通过时，改写下面的「全量」，只留最近一次。写明日期、平台、线程模式、通过项数，以及机器的 CPU 型号和实测频率。只跑了部分用例不要写成全量通过。操作步骤仍在 [testing.md](testing.md)。

## 应用启动与错误诊断验收（2026-11-27）

本轮受测 HEAD `b5a27ecca98f9053d72fb851bfdde38f482def60` 加工作区改动；四路全量的 poll 基线不完全相同，不能记为拟提交树四路全量。Windows MT/ST 和远程 Linux MT 全量含本地未提交的 `oi_poll.cpp` 差异（SHA-256 `50c2e641123e266e60a3f23a9694ff9d3d30227473127c7715f57c45f2ae0016`）；远程 Linux ST 全量及其余三路定向补验使用 HEAD 版 poll（SHA-256 `5586b5860e9f783ff230c96b9111cc354c3181baaecff0920b3c5c868f91c3b9`）。

- Windows MSVC x64 Release MT/ST 全量矩阵均通过，分别 25 项通过、Unix 专属 `tcp-error-rearm` 跳过；耗时 681.7/685.2 秒，CPU Intel Core i9-11900K，测后 CIM 3504 MHz。结果在 `.local/appopt-final-20261127/windows-mt`、`.local/appopt-final-20261127/windows-st`。两路均实际输出第二监听失败后释放及重启、Web 上游异常的通过标记。
- 远程 Linux GCC 13 Release MT 的工作树全量 26/26 及 ASan/UBSan 通过，CPU Xeon Platinum 8255C，测后采样 2494.140 MHz；结果为 `~/openidea-test-results/appopt-final-mt-20261127`。HEAD 版 poll 另跑 hold、web、tcp-error-rearm 均通过，结果分别为 `~/openidea-test-results/appopt-commit-mt-hold`、`appopt-commit-mt-web`、`appopt-commit-mt-tcp`。
- 远程 Linux GCC 13 Release ST 的拟提交树全量 26/26 及 ASan/UBSan 通过，CPU Xeon Gold 6271C，测后采样 2593.904 MHz；结果为 `~/openidea-test-results/appopt-commit-st-20261127`。其中 hold、web、tcp-error-rearm 均通过。
- Windows MT/ST 的拟提交树 hold、web 业务均通过，结果在 `.local/appopt-commit-windows-{mt,st}-{hold,web}`。ST hold 矩阵的配置探针一次返回 41 但输出为空，web 矩阵重测通过；独立重复两种参数顺序各 100 次时，`--test-hooks --config` 出现 2 次相同空输出，原因未确认，证据在 `.local/appopt-commit-windows-st-config-repeat.json`。
- 首轮四路因第二监听失败进程的预期非零退出被清理器误判而失败；修复后第二轮业务均通过，但旧安全单测未 mock 新增探针而失败。更新 mock 后上述最终矩阵和定向检查通过，原结果保留。四路前后按归属清理通过。
- 四路业务 Redis 用例均通过；安全脚本单元测试中未传 Redis 可执行文件的单项跳过不是业务 Redis 跳过。本轮 Windows、Linux MT/ST 均未运行 MySQL 或静态路由专项。Linux 两路全量运行 ASan/UBSan；Windows 不运行该 Linux 探针。

## 日志文件与测试工具复验（2026-11-26，本轮矩阵未四路通过）

本轮四路业务 `all` 均通过，日志跨日、断连合同、慢盘和写满定向用例也在 Windows、远程 Linux 的单/多线程通过；不能据此把四路矩阵记为全量通过。此前「全量」记录保留。

- Windows MT 矩阵通过，耗时 672.1 秒；ST 业务全量通过，但矩阵因 `parallel-lane-tests` 的真实 robocopy 同时间戳覆盖失败而失败，耗时 694.5 秒。结果在 `.local/todo8-stable-windows-mt` 和 `.local/todo8-stable-windows-st`；Windows CPU 为 Intel Core i9-11900K，测试中 CIM 3504 MHz。修复后工具单项两模式另行通过，不抹去原矩阵失败。
- 远程 Linux MT/ST 的 26 项业务、MySQL、静态路由和 ASan/UBSan 通过，但两路 `test-safety` 在 Python 3.12 因静态嵌套层数过多而失败，矩阵 `passed=false`、`completed=false`。MT/ST 修复后 safety 单独均通过（各 48 项，3 跳过），清理通过；ST 日志为 `st-safety-retest-20261126.log`。两机 localhost 双栈回退子检查因地址族环境受限未覆盖。MT CPU Xeon Platinum 8255C 2494.140 MHz，ST CPU Xeon Gold 6271C 2593.904 MHz；结果分别为 `mt-closure-full-20261126-194746`、`st-closure-full-20261126-194801`。
- 两平台各路归属清理通过。Windows 真 MySQL 和静态路由两模式通过，记录在 `.local/mysql-final-windows-mt`、`.local/mysql-final-windows-st`。Windows ST 的阻塞唤醒专项为 100000 样本，trace 和 native 完整，结果含 `wake-metrics.json`；本轮不以不同机器的耗时设阈值。
- 唤醒性能单线程专项：远程 Linux 的 100000 次串行空任务三轮平均分别为 13.590、13.840、13.800 微秒；真 MySQL 的 `mysql_direct_finish_to_resume` 为 1000 样本，p50/p95/p99 分别 15.241/25.457/32.247 微秒，逐段结果在 `st-wake-20261126-200353`。Windows ST 同一轮的串行空任务平均 39.265 微秒；独立 trace 的 `trace_total` p50/p95/p99 为 36.1/69.2/96.5 微秒。串行整链与独立打点采样分别记录，不将两者相减或混为同一测量。
- Windows `web-stop` 单/多线程各做两轮 control、idle、stop：control 每轮完整接收 9437184 字节；idle 零退出后收到 RST；活跃响应停服每轮收到 8185 字节后 RST，诊断失败。结果位于 `.local/webstop-final-windows-mt`、`.local/webstop-final-windows-st`。这是已观察行为，停服是否应排空在途响应仍待定。

## 全量（2026-11-28，数据库接口）

受测 HEAD `0ee39708a255ed8a9fc9dd8a8b27a3b7c1a54a0f` 加工作区改动。四路 `all` 均通过，20 个非文档文件的 LF 归一化清单 SHA256 为 `0455631f00931194c741e4e786d545fd215ace3e01f0315db6e30dffff7e3e22`，四路隔离树逐项匹配。两路 WSL 的 ASan/UBSan 通过；四路测试前后按归属清理通过。

| 平台 | 线程 | CPU | 频率 MHz | `all` 结果 | 耗时 |
| --- | --- | --- | ---: | --- | --- |
| Windows MSVC x64 Release | 多线程 | AMD Ryzen 7 9800X3D | 4700（测后） | 25 通过、1 Unix 专项跳过 | 541.5 秒 |
| Windows MSVC x64 Release | 单线程 | AMD Ryzen 7 9800X3D | 4700（测后） | 25 通过、1 Unix 专项跳过 | 574.8 秒 |
| WSL Ubuntu 24.04 GCC 13.3 Release | 多线程 | AMD Ryzen 7 9800X3D | 4699.888 | 26/26，21 步骤通过 | 338.9 秒 |
| WSL Ubuntu 24.04 GCC 13.3 Release | 单线程 | AMD Ryzen 7 9800X3D | 4699.888 | 26/26，20 步骤通过 | 295.6 秒 |

Windows 跳过项为 Unix libuv POLLERR 专项 `tcp-error-rearm`；两路均输出 `hold captured tasks done=32 bad=0`、`web PASS` 和 `web-coro PASS`。结果目录为 `.local/db-isas-final-windows`、`.local/sql-final-mt6/linux-mt`、`.local/db-final6-linux-st/linux-st`。

独立真 MySQL 和静态注册四路通过。Windows 两路均输出 `mysql cell predicates bad=0`、`mysql row mapping bad=0`、`mysql all field types batch bad=0`、`mysql rows bad=0`、`mysql error handling bad=0`、`mysql done bad=0`；日志实际输出的原生码有 SQL 1054、认证 1045、连接 2002，重复键 1062 由断言检查而非单独日志输出。Windows 真库结果为 `.local/db-isas-win-mt-mysql/mysql/ipcclient.out`、`.local/db-isas-win-st-mysql/mysql/ipcclient.out`。专项前后归属清理通过。

前测 `.local/db-api-final-windows` 两路因注册服务仍用旧数据库接口构建失败，后已迁移；`.local/db-api-v2-windows` 业务通过但旧字段名称断言失败，后已同步；`.local/db-alltypes-windows` 两路因嵌套 JSON 容器的重载查找构建失败，后已修复并保留触发用例。另一次旧组合前测按版本切换中断，均不计本轮通过。

## 收包额度与解码预算复验（2026-11-25 晚，历史记录）

受测 HEAD `1ce19f1a4c33b1751b80dd2b583801f388dcd835` 加工作区改动：默认接收额度恢复 64 MiB + 8、解码预算取 max(64 MiB, 4 × 帧上限)、redisserver 超限拒绝日志、`test_config_macros.py` 在 Linux 固定 g++-13，以及对应用例与文档。四路都由 `tools/parallel_matrix.py` 跑 `--case all`，同时开跑同一份源码。Linux 用本机 WSL（Ubuntu 20.04，GCC 13.1），不是远程机器；WSL 自带 OpenSSL 1.1.1，按 [testing.md](testing.md) 第 2 节用 `/root/opt/openssl-3.0.18` 静态链接。

| 平台 | 线程 | CPU | 频率读数 MHz | `all` 结果 | 矩阵状态 | 耗时 |
| --- | --- | --- | --- | ---: | --- | --- |
| Windows | 多线程 | Intel Core i9-11900K | 未另采 | **21/22，失败** | registry 业务 PASS 后临时目录清理 WinError 5；exit=1、passed=false、completed=false | 582.7 秒 |
| Windows | 单线程 | Intel Core i9-11900K | 未另采 | 22/22 | exit=0、passed=true、completed=true | 609.8 秒 |
| WSL Linux | 多线程 | Intel Core i9-11900K | 3503.997（`/proc/cpuinfo` 采样） | 22/22 | exit=0、passed=true、completed=true | — |
| WSL Linux | 单线程 | Intel Core i9-11900K | 3503.997（同上） | 22/22 | exit=0、passed=true、completed=true | — |

四路都出现本轮新断言的成功标记：`hold decode budget scaled words=1400000 charged=84000000 frame=44800326 code=0 bad=0`、`hold receive limit configured receive=1048576 frame=1048568 load=0`、`rpc default receive exact=67108872 over=67108873 PASS`、`rpc configured receive exact=1048576 over=1048577 PASS`、`redis oversized command rejected limit=1048576 PASS`、`hold batch bind failure caught=1 registered=1 submits=1 drops=1 elapsed=... bad=0`，以及汇总小预算边界、16 MiB 放大和 6000 字节容量检查的 `hold binary container bad=0`。

失败路径：把 `SetDecodeBudget` 那一行临时去掉、其余不变，Windows 多线程 hold 输出 `hold decode budget scaled ... code=-1 bad=1`，服务端 `decode_request ... reason=ParserException`，用例失败；恢复后通过。

**Windows MT 清理失败边界**：`registry PASS seconds= 0.23` 之后 `TemporaryDirectory.cleanup` 删除 `oi-regression-p0-f5oql9zw\bin\ipcclient.exe` 报 `[WinError 5]`，其后 `udp`、`multi`、`local`、`web`、`web-coro` 业务均 PASS。目录留存 25 文件、30,978,331 字节；事后 CIM 无该目录下的进程，独占打开 `ipcclient.exe` 成功，只说明事后无持续占用，报错瞬间的占用者未取到。结果目录：`.tmpfiles/win-par/`、`.tmpfiles/wsl-par2/`（工作区临时目录，不入库）。

本轮未跑 ASan、coro、mysql、registry-static。

## 最终排版后复验（2026-11-25，历史记录）

当时 Windows ST、远程 Linux MT/ST 三路 22/22，Windows MT 21/22。

**Windows MT 清理失败**：registry 业务 PASS 后，`TemporaryDirectory.cleanup` 删除临时客户端 exe 报 `[WinError 5] Access is denied`；官方前后归属清理均 exit=0、残留进程数 0，但不能抵消目录清理失败。保留目录 `C:/Users/Admin/AppData/Local/Temp/oi-regression-p0-cs6eqfri`，**25 文件、30,969,999 字节（约 30.97 MB）**。事后 5 个归属 PID 均不存在、CIM 无同目录进程，非只读且 ACL FullControl，独占读/DELETE 开句柄成功、Restart Manager 无占用者；只证明事后无持续占用，**报错瞬间原因未知**，不猜杀毒、不视为已修。身份与留存证据见 [main-vs-tested](<C:/Users/Admin/openidea-test-results/resume-20261125-final/par-windows-mt/main-vs-tested.json>)、[lane-after-identity](<C:/Users/Admin/openidea-test-results/resume-20261125-final/par-windows-mt/lane-after-identity.json>)、[source-after](<C:/Users/Admin/openidea-test-results/resume-20261125-final/par-windows-mt/source-after.json>)、[retained-registry-directory](<C:/Users/Admin/openidea-test-results/resume-20261125-final/par-windows-mt/retained-registry-directory.json>)。不改脚本、不再重跑刷绿。

**其他门禁边界**：全仓 clang-format 18.1.8 检查仍有 **13 份 HEAD 既有格式失败**，不能声称全仓格式通过。

## 显式用例

### 回归验收专项（2026-11-25 早先历史记录，本轮未重跑）

Windows 多线程和单线程的 `coro` 定向用例均完成 10 万次挂起唤醒、100 万个空任务并报告 `bad=0`；Windows 单线程实际计数为 held=100000、wake=100000、done=1000000。Windows HTTP 保留响应缓冲的故障负例输出 `retained=63 bad=1`。Linux 单线程启用测试钩子后，唤醒分段七项各有 100000 个样本；删除标记后实际运行失败，随后恢复 OFF 构建。上述专项独立于全量 22 项。


### 时间轮、HTTP 与配置定向验证（2026-11-24）

本轮不是同一最终源码的四路全量。旧时间轮级联语义的对照出现目标 tick 20、实际 tick 21；新版边界回归检查 18 次准点执行且不重入，Linux 多线程输出 `hold timer boundaries phase=3 calls=18 bad=0`。Windows 的 localhost 双栈回退专项耗时 868.8 ms；两台远程 Linux 的 localhost 只解析到 IPv4，不能在这两台验证双栈回退。

较早的 Linux 多线程、单线程各跑了 22 项业务全量以及 MySQL 和静态注册发现；MySQL 的行映射、行操作和客户端检查均为 `bad=0`。多线程那轮安全检查因测试脚本语法失败，修正后使用相同 Redis 配置单独重跑 45 项通过，原矩阵汇总仍为失败；单线程矩阵汇总通过。这两轮早于最后的配置读取调整，不能当作最终源码全量。

随后对最终直接读取实现做定向验收：Linux 两种线程模式的配置宏兼容和整数越界测试、两个定时器整数配置非法值的启动拒绝及缺省启动均通过；多线程另验证旧线程键拒绝和时间轮专项，均通过。

Linux 单线程旧续期行为对照使用同一正式积压探针、50ms tick，以测试访问回拨单调时间基点 220ms：三次 Update 后的累计回调数为 `3/3/3`，首次补跑三次，探针输出 `bad=1`、退出 255。新版 Linux 多线程与单线程分别输出 `first=1 second=2 final=3 child_first=0 child=1 stop=1 ticks=1/6 bad=0`：每次 Update 后累计次数依次为 1、2、3，首次回调新登记的 1ms 定时器不在首次 Update 补跑，Stop 只执行一次。探针在首次回调中实际等待 60ms；此定向故障注入不代表生产 poll 线程允许阻塞。

2026-11-21，同一工作区跑了不进 `--case all` 的协程、数据库和序列化。都是退出码 0。这不是全量。回声没有重跑。Linux 用记录中的远程机器。本机 Windows 频率沿用上面全量记录的 CIM 值，本轮没有另采。

数据库功能用例四路都是 `mysql row mapping bad=0`、`mysql rows bad=0`、`mysql done bad=0`。

### 唤醒路径拆分（2026-11-24，单线程）

本次 Windows 为 Intel Core i9-11900K（8 核 16 线程，报告频率 3504 MHz），Linux 为 2 核 Intel Xeon Gold 6271C（报告频率 2593.904 MHz）；两者均为 Release、单线程、测试钩子开启。各独立三轮，使用默认池空回调，每组预热 100 次、采样 100000 次。此专项不代表全量回归。未打点整链计时和分段采样是独立运行；分段以单线程测试钩子记录同一请求的提交前、工人开始、工人完成、通知发布、poll 接收、业务恢复，逐次检查顺序与段和。原生对照由后台协调线程和专用工人通过互斥锁及条件变量按轮次交接，边界不同，不能把两者差值直接归因于框架或 libuv。

未打点空回调整链的总耗时，单位毫秒：

| 平台 | 轮次 | 空回调整链 |
| --- | ---: | ---: |
| Windows | 1 | 3583.804 |
| Windows | 2 | 6096.731 |
| Windows | 3 | 4453.205 |
| Linux | 1 | 1371.137 |
| Linux | 2 | 1384.351 |
| Linux | 3 | 1352.748 |

以下每项均为 100000 个配对样本。均值由日志 `sum_us / n` 计算；总和单位毫秒，其余时间单位微秒，≥1ms 为该项样本数。每轮单独保留，分位数不能跨轮相加或平均。

| 平台 | 轮次 | 指标 | 总和 ms | 均值 µs | p50 | p95 | p99 | max | ≥1ms |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Windows | 1 | 提交→工人开始 | 611.212 | 6.112 | 5.500 | 8.400 | 16.300 | 187.000 | 0 |
| Windows | 1 | 工人执行 | 12.021 | 0.120 | 0.100 | 0.200 | 0.300 | 30.900 | 0 |
| Windows | 1 | 工人完成→发布 | 10.070 | 0.101 | 0.100 | 0.100 | 0.200 | 10.500 | 0 |
| Windows | 1 | 发布→poll | 1908.540 | 19.085 | 17.100 | 29.700 | 37.900 | 4414.100 | 2 |
| Windows | 1 | poll→恢复 | 1189.081 | 11.891 | 11.700 | 20.600 | 27.000 | 288.900 | 0 |
| Windows | 1 | 分段整次 | 3730.924 | 37.309 | 34.300 | 58.400 | 78.500 | 4452.800 | 2 |
| Windows | 1 | 协调→工人 | 44.065 | 0.441 | 0.300 | 1.000 | 1.300 | 215.500 | 0 |
| Windows | 1 | 工人→协调 | 62.018 | 0.620 | 0.600 | 1.200 | 1.400 | 132.100 | 0 |
| Windows | 1 | 原生往返 | 106.083 | 1.061 | 1.000 | 1.700 | 2.200 | 221.200 | 0 |
| Windows | 2 | 提交→工人开始 | 744.807 | 7.448 | 6.800 | 10.300 | 15.800 | 2590.100 | 3 |
| Windows | 2 | 工人执行 | 14.768 | 0.148 | 0.100 | 0.200 | 0.300 | 177.700 | 0 |
| Windows | 2 | 工人完成→发布 | 10.144 | 0.101 | 0.100 | 0.200 | 0.300 | 35.200 | 0 |
| Windows | 2 | 发布→poll | 2317.658 | 23.177 | 22.300 | 37.600 | 48.000 | 393.900 | 0 |
| Windows | 2 | poll→恢复 | 1523.721 | 15.237 | 13.700 | 26.500 | 38.600 | 742.100 | 0 |
| Windows | 2 | 分段整次 | 4611.099 | 46.111 | 43.900 | 73.200 | 103.500 | 2939.900 | 3 |
| Windows | 2 | 协调→工人 | 46.262 | 0.463 | 0.300 | 1.100 | 1.400 | 145.900 | 0 |
| Windows | 2 | 工人→协调 | 61.040 | 0.610 | 0.600 | 1.200 | 1.400 | 296.300 | 0 |
| Windows | 2 | 原生往返 | 107.302 | 1.073 | 1.000 | 1.700 | 2.300 | 297.300 | 0 |
| Windows | 3 | 提交→工人开始 | 913.785 | 9.138 | 7.100 | 11.200 | 29.000 | 8730.400 | 29 |
| Windows | 3 | 工人执行 | 15.242 | 0.152 | 0.100 | 0.200 | 0.400 | 28.200 | 0 |
| Windows | 3 | 工人完成→发布 | 11.201 | 0.112 | 0.100 | 0.200 | 0.300 | 86.700 | 0 |
| Windows | 3 | 发布→poll | 2539.442 | 25.394 | 23.500 | 40.800 | 62.900 | 3317.700 | 7 |
| Windows | 3 | poll→恢复 | 1703.092 | 17.031 | 15.300 | 29.500 | 52.200 | 2091.800 | 4 |
| Windows | 3 | 分段整次 | 5182.762 | 51.828 | 47.200 | 81.400 | 142.000 | 8971.100 | 52 |
| Windows | 3 | 协调→工人 | 46.401 | 0.464 | 0.300 | 1.000 | 1.400 | 154.600 | 0 |
| Windows | 3 | 工人→协调 | 64.421 | 0.644 | 0.600 | 1.200 | 1.400 | 171.000 | 0 |
| Windows | 3 | 原生往返 | 110.821 | 1.108 | 1.000 | 1.700 | 2.300 | 171.300 | 0 |
| Linux | 1 | 提交→工人开始 | 576.979 | 5.770 | 5.477 | 5.878 | 12.222 | 1917.202 | 1 |
| Linux | 1 | 工人执行 | 7.995 | 0.080 | 0.047 | 0.221 | 0.237 | 9.475 | 0 |
| Linux | 1 | 工人完成→发布 | 6.462 | 0.065 | 0.061 | 0.071 | 0.216 | 8.861 | 0 |
| Linux | 1 | 发布→poll | 553.796 | 5.538 | 5.313 | 5.600 | 11.894 | 1047.641 | 1 |
| Linux | 1 | poll→恢复 | 269.365 | 2.694 | 2.584 | 3.023 | 7.366 | 73.321 | 0 |
| Linux | 1 | 分段整次 | 1414.596 | 14.146 | 13.522 | 14.749 | 23.034 | 1938.280 | 3 |
| Linux | 1 | 协调→工人 | 433.165 | 4.332 | 4.172 | 4.310 | 9.730 | 1023.270 | 1 |
| Linux | 1 | 工人→协调 | 429.665 | 4.297 | 4.149 | 4.306 | 9.587 | 1011.555 | 1 |
| Linux | 1 | 原生往返 | 862.830 | 8.628 | 8.323 | 8.622 | 15.630 | 1030.148 | 2 |
| Linux | 2 | 提交→工人开始 | 551.723 | 5.517 | 5.359 | 5.752 | 11.602 | 260.054 | 0 |
| Linux | 2 | 工人执行 | 7.786 | 0.078 | 0.059 | 0.207 | 0.225 | 24.861 | 0 |
| Linux | 2 | 工人完成→发布 | 6.252 | 0.063 | 0.051 | 0.187 | 0.210 | 7.433 | 0 |
| Linux | 2 | 发布→poll | 535.398 | 5.354 | 5.197 | 5.482 | 11.614 | 264.419 | 0 |
| Linux | 2 | poll→恢复 | 279.011 | 2.790 | 2.789 | 3.032 | 3.411 | 70.218 | 0 |
| Linux | 2 | 分段整次 | 1380.170 | 13.802 | 13.428 | 14.174 | 21.942 | 282.872 | 0 |
| Linux | 2 | 协调→工人 | 428.426 | 4.284 | 4.133 | 4.262 | 9.547 | 850.755 | 0 |
| Linux | 2 | 工人→协调 | 429.054 | 4.291 | 4.140 | 4.237 | 9.636 | 698.830 | 0 |
| Linux | 2 | 原生往返 | 857.480 | 8.575 | 8.274 | 8.566 | 15.358 | 856.716 | 0 |
| Linux | 3 | 提交→工人开始 | 559.437 | 5.594 | 5.406 | 5.790 | 11.659 | 269.819 | 0 |
| Linux | 3 | 工人执行 | 8.204 | 0.082 | 0.043 | 0.212 | 0.232 | 10.104 | 0 |
| Linux | 3 | 工人完成→发布 | 5.785 | 0.058 | 0.057 | 0.068 | 0.079 | 6.961 | 0 |
| Linux | 3 | 发布→poll | 532.530 | 5.325 | 5.167 | 5.452 | 11.254 | 355.520 | 0 |
| Linux | 3 | poll→恢复 | 267.814 | 2.678 | 2.604 | 2.970 | 3.334 | 107.760 | 0 |
| Linux | 3 | 分段整次 | 1373.769 | 13.738 | 13.353 | 14.095 | 21.764 | 369.388 | 0 |
| Linux | 3 | 协调→工人 | 425.668 | 4.257 | 4.103 | 4.219 | 9.662 | 622.673 | 0 |
| Linux | 3 | 工人→协调 | 426.192 | 4.262 | 4.133 | 4.227 | 9.298 | 864.811 | 0 |
| Linux | 3 | 原生往返 | 851.860 | 8.519 | 8.238 | 8.535 | 15.252 | 869.089 | 0 |

### 真实 MySQL 唤醒延迟（2026-11-21，单线程）

Windows 是本机 AMD Ryzen 7 9800X3D。Linux 是远程 2 核 Intel Xeon Gold 6271C，跑之前 `/proc/cpuinfo` 的 `cpu MHz` 为 2593.904。三轮分开跑，没有合并。

真实 MySQL 每轮预热 100 次、采样 1000 次，单位微秒。直接 SQL 完成到恢复是主要指标。Reset 后界到恢复包含后续清理，是上界。整次调用也是上界，不并进直接指标。

| 平台 | 轮次 | 指标 | p50 | p99 | max | ≥1ms |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| Windows | 1 | 直接 SQL 完成到恢复 | 29.600 | 92.300 | 328.900 | 0 |
| Windows | 1 | Reset 后界到恢复 | 29.000 | 84.900 | 144.800 | 0 |
| Windows | 1 | 整次调用 | 375.300 | 849.900 | 1146.900 | 3 |
| Windows | 2 | 直接 SQL 完成到恢复 | 29.400 | 78.100 | 228.600 | 0 |
| Windows | 2 | Reset 后界到恢复 | 31.300 | 72.200 | 98.100 | 0 |
| Windows | 2 | 整次调用 | 391.800 | 762.100 | 950.100 | 0 |
| Windows | 3 | 直接 SQL 完成到恢复 | 31.400 | 78.100 | 152.100 | 0 |
| Windows | 3 | Reset 后界到恢复 | 31.200 | 92.500 | 182.400 | 0 |
| Windows | 3 | 整次调用 | 379.500 | 771.700 | 904.300 | 0 |
| Linux | 1 | 直接 SQL 完成到恢复 | 14.951 | 28.730 | 80.182 | 0 |
| Linux | 1 | Reset 后界到恢复 | 15.906 | 30.009 | 508.001 | 0 |
| Linux | 1 | 整次调用 | 294.030 | 417.855 | 2599.915 | 1 |
| Linux | 2 | 直接 SQL 完成到恢复 | 14.909 | 28.003 | 110.519 | 0 |
| Linux | 2 | Reset 后界到恢复 | 15.717 | 28.934 | 262.463 | 0 |
| Linux | 2 | 整次调用 | 294.148 | 444.617 | 1045.748 | 2 |
| Linux | 3 | 直接 SQL 完成到恢复 | 15.150 | 27.586 | 219.062 | 0 |
| Linux | 3 | Reset 后界到恢复 | 16.134 | 28.476 | 32.480 | 0 |
| Linux | 3 | 整次调用 | 297.034 | 407.488 | 1825.179 | 2 |

直接路径的基准是同一次 `SELECT 1` 的三段均值，单位微秒，每轮 1000 次。整段是三段相加。排队是 `co_await` 之前到工人开始执行。SQL 是已经连上的连接上 `Execute` 再 `Reset`。结束到恢复从回调记下的结束时刻到协程恢复，里面是 socketpair 通知、poll 唤醒和把协程拉起来，这三步没有再拆。均值是该轮 `sum_us / n`，三轮没有再平均。

| 平台 | 轮次 | 整段 | 排队 | SQL | 结束到恢复 | SQL 占比 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Windows | 1 | 416.2 | 8.6 | 373.1 | 34.5 | 89.6% |
| Windows | 2 | 404.7 | 8.4 | 363.0 | 33.2 | 89.7% |
| Windows | 3 | 422.7 | 8.6 | 379.5 | 34.6 | 89.8% |
| Linux | 1 | 294.5 | 8.6 | 270.2 | 15.7 | 91.7% |
| Linux | 2 | 292.8 | 8.3 | 268.9 | 15.6 | 91.8% |
| Linux | 3 | 298.6 | 9.3 | 273.4 | 15.9 | 91.5% |

唤醒基准取结束到恢复。Windows 三轮均值 34.5、33.2、34.6 微秒，p50 为 29.6、29.4、31.4 微秒。Linux 三轮均值 15.7、15.6、15.9 微秒。时间主要在 SQL，排队大约 2% 到 3%。真正的 `Db::Client::Call` 整段均值：Windows 412.3、413.8、406.7 微秒，Linux 303.5、302.1、303.2 微秒。

### 回声

本轮没有重跑。上一笔仍是远程 4 核多线程：100 万次，`ok=1000000 mismatch=0 rpc_fail=0`，12.9 秒，约 77453 次/秒。

### 协程

2026-11-21 重跑了本机 Windows 和两台远程。三段端到端：`live`（`StartTask` 到全部挂起）、`wake`（`Schedule` 到跑完）、`churn`（空任务 `StartTask` 到跑完）。纳秒/条由日志直接给出。挂起路径的 `StartTask→跑完` = `10^9 / (live + wake)`。空任务速率用日志里的 `churn_rate`。Windows 内存是日志里的 `bytes_per`。本机 WSL 两行是更早的测量，这次没有重跑。远程 2 核测完仍是单线程构建。

| 平台 | 线程 | 工人 | 每条内存（字节） | live（纳秒/条） | wake（纳秒/条） | 挂起路径 StartTask→跑完（条/秒） | churn（纳秒/条） | 空任务 StartTask→跑完（条/秒） |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 远程 4 核 | 多线程 | 3 | 201 | 2137 | 420 | 391083 | 960 | 1041666 |
| 远程 2 核 | 单线程 | 1 | 177 | 194 | 67 | 3831418 | 213 | 4694835 |
| 本机 Windows | 单线程 | 1 | 211 | 177 | 86 | 3802281 | 263 | 3802281 |
| 本机 Windows | 多线程 | 3 | 252 | 420 | 150 | 1754386 | 298 | 3355704 |
| 本机 WSL（未重跑） | 单线程 | 1 | 192 | 69 | 25 | 10638297 | 77 | 12987012 |
| 本机 WSL（未重跑） | 多线程 | 3 | 197 | 4339 | 135 | 223513 | 268 | 3731343 |

远程 4 核多线程的 `live` 仍比单线程慢一个数量级。Windows 多线程 `live` 也比单线程慢一截。批量 `StartTask` 抢共享队列锁，问题记在 [todo.md](../todo.md)「多线程协程批量创建/入队偏慢」，回头再优化。

### 序列化

2026-11-21。1000 条协议，每条 128 个 `int32_t`，各跑 1000 万次。远程 4 核用 g++-13 `-O2 -std=c++20 -DNDEBUG`，跑之前 `cpu MHz` 为 2494.140。本机 Windows 用 MSVC `/O2`，和 gcc 的数不能横比。只跑了宏字段表（`array`）和 `switch`。细节见 [字段分发](pb-dispatch.md)。

| 机器 | 方案 | 编译（秒） | 编码（纳秒） | 解码（纳秒） |
| --- | --- | ---: | ---: | ---: |
| 远程 4 核 gcc | 宏 | 388.03 | 3125 | 1577 |
| 远程 4 核 gcc | switch | 1939.63 | 3110 | 572 |
| 本机 Windows MSVC | 宏 | 43.209 | 1229 | 599 |
| 本机 Windows MSVC | switch | 79.456 | 1187 | 1206 |
