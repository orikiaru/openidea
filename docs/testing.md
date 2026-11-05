# 跨平台全量测试操作规程

面向维护者与 AI：按本文件执行，不要另写客户端，不要跳过失败，不要把只编译、WSL 运行或强杀退出算成 Windows 原生测试通过。

## 1. 范围与通过条件

- Windows 原生 MSVC x64 Release、Linux WSL GCC 13，各运行多线程 MT 和单线程 ST，共四组。
- ST/MT 只切换 ipcclient、ipcserver；logd 固定多线程，redisserver/registry 按各自配置单线程。
- `all` 包含 18 项：echo、log、log-backpressure、redis、redis-limit、redis-shutdown、redis-queue-timeout、rpc-ingress、redis-backpressure-bytes、redis-backpressure-count、redis-fairness、hold、rpc-admission-count、rpc-admission-bytes、reset、disk-full、registry、web。
- Linux 可额外运行 clock-forward、clock-back，以及纯协程 ASan。校时故障只影响测试子进程，绝不调整宿主机时钟。
- 必须全部返回 0；不能用删除断言、扩大超时、提高容量或重试掩盖失败。偶发失败保留日志，记录复测结果及根因是否确认。
- 本次源码正在变化时不要声称覆盖了最新版本；核对结果目录的 environment.json、Git HEAD、diff 摘要后再报告。

## 2. 环境预检

仓库路径可以包含空格。以下示例在仓库根目录执行，示例中的用户、构建目录和 Redis 路径按实际环境传参。

Windows：Python 3.10+、Visual Studio 2022 的“使用 C++ 的桌面开发”（MSVC v143 x64、MSBuild、Windows SDK）、CMake。仅有 cl.exe 不够，还要有 MSVC include/lib 与 SDK include/lib。脚本会寻找 PATH 中的 CMake，也能找到 VS 附带的 CMake。

Linux：WSL 已配置的 Linux 用户、Python 3.8+、cmake、gcc-13/g++-13、make、已编译的 Redis 服务端。源码编译 Redis 可使用已有的 `tools` 外部依赖目录，默认路径 `/tmp/redis-7.4.12/src/redis-server`；路径不存在必须提供 `--redis-binary`，脚本不会下载不明 Redis 二进制。

测试占用本机端口 19995、17981、16379、16380、17990、17992、17993。测试 Redis 用 16380，不用 6379：Windows 动态端口段可能被 Hyper-V/WinNAT 保留，本机已有的 6379 Redis 也不会和测试冲突。复制出的 `redisserver.toml`、`registry.toml` 由脚本改到 16380，仓库里的配置仍是 6379。运行前脚本探测占用；不杀其他人的服务。同一份工作目录内不要并行；要并行就用 `tools/parallel_matrix.py`，它把工作目录同步成四份（Windows 在 `F:/oi-par`，Linux 在 WSL 的 `/root/oi-par`），Windows MT/ST、Linux MT/ST 各用一份，`--port-offset` 依次为 0、1000、2000、3000，所有测试端口整体平移，互不抢占：

```powershell
python tools/parallel_matrix.py --windows-redis-binary C:/tools/redis/redis-server.exe --artifacts C:/Users/Admin/openidea-test-results/parallel
```

`--lane windows-mt` 之类只跑其中一路，可分给多个 subagent 各跑一路。临时目录前缀带偏移（`oi-regression-p<偏移>-`），每路清理进程只匹配自己那份工作目录和自己的前缀，不要动别的路。每份各自编译、测试并恢复 MT，不碰主工作目录的 `deploy/bin`；改动跑通后主目录仍要按第 3 节编回 MT。

Windows 侧测试 Redis 用 Windows 原生 `redis-server.exe`（[redis-windows](https://github.com/redis-windows/redis-windows/releases) 的 msys2 便携包，解压即用，不装服务），`--platform both` 用 `--windows-redis-binary` 指定，直接跑 `run_regressions.py` 时用 `--redis-binary`。不要再用 `--redis-wsl`：WSL 的 localhost 转发和 NAT 内网地址在大块双向流量下都会卡死（纯 Python 收发也复现，64MB 下行只收到 3.6MB 就停住），Redis 回压、hold、registry 会假失败。怀疑链路时先用不经被测代码的探针：`tools/diag/redis_link_probe.py --native <exe>` 或 `--wsl [--host <WSL 地址>]` 测大块 ECHO 和 BLPOP 后 PING；`tools/diag/tcp_bulk.py` 测纯 TCP 下行、上行、双向各 64MiB，输出 `STALL` 即链路问题。Redis 为脚本独立启动的临时实例，内存上限 1536MiB、关闭持久化，绝不借用现有数据库。

## 3. 推荐的一键矩阵

### Windows 原生 Redis 下载

没有本地 Redis 时，从上面链接的项目 Release 下载便携包，解压到仓库外，不注册系统服务。本轮使用 `8.10.3` 的 `Redis-8.10.3-Windows-x64-msys2.zip`，下载及解压示例：

```powershell
$redisTools = Join-Path $env:USERPROFILE 'tools/redis-8.10.3'
New-Item -ItemType Directory -Force -Path $redisTools | Out-Null
$redisArchive = Join-Path $redisTools 'Redis-8.10.3-Windows-x64-msys2.zip'
Invoke-WebRequest 'https://github.com/redis-windows/redis-windows/releases/download/8.10.3/Redis-8.10.3-Windows-x64-msys2.zip' -OutFile $redisArchive
Get-FileHash -LiteralPath $redisArchive -Algorithm SHA256
Expand-Archive -LiteralPath $redisArchive -DestinationPath $redisTools
$redisBinary = Join-Path $redisTools 'Redis-8.10.3-Windows-x64-msys2/redis-server.exe'
python tools/diag/redis_link_probe.py --native $redisBinary
```

本轮下载文件的 SHA256 为 `af16df340525b431e9ec8dabfd625d5e7a233f82805b5418819904ea909c447f`，用于复现文件身份，不代表发布方签名验证。探针仅操作自己启动的实例：核对启动日志、随机密码认证，并按进程句柄关闭；端口已占用就失败，不发送针对该端口的 `SHUTDOWN`。WSL 探针可用 `--wsl --wsl-user jxy --wsl-binary /tmp/redis-7.4.12/src/redis-server`。

### 执行矩阵

Windows 宿主 PowerShell，同一次调用顺序运行 Windows、Linux：

```powershell
python tools/test_matrix.py --platform both --wsl-user jxy --windows-redis-binary C:/tools/redis/redis-server.exe --asan --clock-faults
```

只跑 Windows：

```powershell
python tools/test_matrix.py --redis-binary C:/tools/redis/redis-server.exe --build-dir build-win
```

只跑 Linux，在 WSL：

```sh
python3 tools/test_matrix.py --build-dir "$HOME/openidea-build-gcc13" --asan --clock-faults
```

自定义路径示例：

```powershell
python tools/test_matrix.py --platform both --wsl-user jxy --linux-build-dir /home/jxy/openidea-build-gcc13 --redis-binary /tmp/redis-7.4.12/src/redis-server --windows-redis-binary C:/tools/redis/redis-server.exe --artifacts C:/Users/Admin/openidea-test-results/manual-run
```

矩阵脚本会：记录源码身份 → 配置 MT → 编译所有模块 → 跑全部用例 → 配置 ST → 编译 → 跑同样用例 → 可选扩展测试 → 在 finally 中恢复 MT 且关闭测试注入并重新编译。任何一步失败，最终退出码非零。平台间串行运行。

默认结果在用户目录 `openidea-test-results/<时间>/`，支持 `--artifacts`。不要将批量日志数据提交进 Git。

## 4. 单项复现与断言

建议用矩阵入口，避免误测过期二进制：

```sh
python3 tools/test_matrix.py --case hold --mode all
```

明确已经构建好时，可以直接运行：

```sh
python3 tools/run_regressions.py --case all --artifacts /tmp/oi-test-artifacts
```

Windows 原生直接运行时，要选择带 exe 的 Release 目录：

```powershell
python tools/run_regressions.py --case all --bin-dir deploy/bin/Release --redis-binary C:/tools/redis/redis-server.exe --artifacts C:/Users/Admin/openidea-test-results/direct
```

Windows 的日志故障测试要求该产物以 `-DOI_ENABLE_TEST_HOOKS=ON` 构建。矩阵入口自动设置和恢复；直接运行入口检查构建标记，缺失则失败，不能跳过。

主要成功标记：

- echo / registry：`ok=1000000 mismatch=0 rpc_fail=0`，连接只启动一次。客户端同时在途的请求数由 `[TestEcho] inflight` 限定（默认 2048），必须低于服务端 `[Rpc] session_max_inflight_count`，否则多线程下会被回 `OI_ERR_BUSY`。
- 客户端时限：跑 hold 那套的用例（含 rpc-admission、clock）120 秒，Redis 压力类和 redis-shutdown 20 秒，其余 60 秒。超时按失败处理并打印客户端输出。
- log：逐条核对实际磁盘记录、字节数和内容；端到端速度以落盘字节除以客户端开始到文件最后增长的时间，不能拿 enqueue_rate 代替。
- redis：`redis integrity ok`、压力统计 `bad=0`；同时验证毫秒 TTL、单次请求等待、脚本隔离、断线、队列超时及限流。
- hold：`hold done bad=0`；覆盖多等待者、异常、RPC 接纳上限、请求生命周期、服务名/分区路由及批量调用。测试脚本额外启动两个 gameserver 分区（1001/1002），不能只起默认 ipcserver 就冒充全量。
- 正常退出必须完成排空和 join。清理超过时限需要强杀，算失败。

## 5. 平台故障注入与清理

Linux 的日志慢写使用 LD_PRELOAD，写满使用 `/dev/full`；Windows 使用仅测试构建启用的日志写线程钩子，分别延迟写入和注入 ENOSPC，实际网络、队列与错误处理仍走真实进程。注入不运行在 poll 线程。生产构建默认关闭 `OI_ENABLE_TEST_HOOKS`。

Windows 原生进程在隐藏控制台启动，通过专用控制台中断辅助进程发出 Ctrl+C，以验证真实正常退出；不会用 `Popen.terminate()` 的强杀伪装排空成功。Redis 压力测试暂停的是本次创建的 Redis 实例；不会暂停现有服务。

每个用例用系统临时目录的 bin/config/logs。复制配置后修改，不写仓库配置；日志超过 2GiB 立即失败。所有子进程、Redis、暂停状态、HTTP 夹具、文件句柄必须在 finally 清理。失败保留文本输出到结果目录，批量日志数据仍删除。中断或机器重启时，先查明遗留目录和进程归属，再清理，不能按进程名杀全机服务。

## 6. 检查工具与结果交接

```sh
python3 tools/test_field_names.py
python3 tools/format_cpp.py --check --clang-format /path/to/clang-format-18.1.8
/path/to/python-with-libclang tools/check_field_names.py --build-dir "$HOME/openidea-build-gcc13" --jobs 4
git diff --check
```

结果目录包含：environment.json（环境、HEAD、diff 摘要）、matrix.json（每一步命令和退出码）、每一步日志、每个用例的进程输出、results.json（逐例通过/失败）、summary.json（整体结果）。同时运行两端时还有 platforms.json。

交给下一位 AI 时必须列出：实际测试的平台/编译器/线程模式、结果目录、失败或阻塞项、是否恢复 MT、是否关闭测试注入、是否清理临时数据。缺 SDK、缺 Redis、编译失败或操作被拦截，都只能记录为阻塞/失败；不能写“全量通过”。

## 7. 四路并行的 subagent 提示词范本

进程清理统一用 `tools/kill_test_procs.py`：按本仓库或本路副本的完整路径，以及临时目录中的 `.oi-test-owner.json` 归属标记匹配。标记记录仓库、端口偏移、PID 及创建标识（Windows FILETIME、Linux `/proc` starttime）。Windows 的路径归属还须核对 CIM 快照创建时间，并通过同一进程句柄验证身份、执行终止。端口、目录前缀和通用脚本名都不能单独证明进程归属。WSL 保留 6379，不执行 `wsl --shutdown`；已知属于测试的进程无法检查、清理错误或有残留，必须返回非零。`--dry-run` 只列不杀。

换机器时，清理和矩阵必须使用相同的 `--windows-root`、`--linux-root`、`--wsl-user`；清理旧的无标记实例时，先人工核实归属，不能退回按端口杀进程。脚本安全回归为 `python tools/test_test_safety.py`，矩阵会自动运行。

先由主对话做一次全局清理 `python tools/kill_test_procs.py`，然后问用户选模型，四个 subagent 同时在后台派发，每个一路，按下表替换范本里的占位符。

| `<LANE>` | `<OFFSET>` | `<OTHERS>` | `<MODE_NOTE>` |
| --- | --- | --- | --- |
| windows-mt | 0 | windows-st / linux-mt / linux-st | 在 F:\oi-par\windows-mt 编译 MT |
| windows-st | 1000 | windows-mt / linux-mt / linux-st | 在 F:\oi-par\windows-st 编译 ST，最后恢复 MT |
| linux-mt | 2000 | windows-mt / windows-st / linux-st | 在 WSL /root/oi-par/linux-mt 编译 MT，Redis 用 /usr/bin/redis-server |
| linux-st | 3000 | windows-mt / windows-st / linux-mt | 在 WSL /root/oi-par/linux-st 编译 ST，最后恢复 MT，Redis 用 /usr/bin/redis-server |

范本：

````text
你在 Windows 宿主上为仓库 F:\openidea 跑回归矩阵的其中一路：**<LANE>**（端口偏移 <OFFSET>）。另外三路（<OTHERS>）正由别的 agent 同时跑，绝不能杀或动它们的进程和目录。WSL 里用户自己的 Redis 在 6379，绝不能杀。只跑测试和报告，不要改仓库任何文件，不要 git commit/push。用中文回报。

## 清理（开始前和结束后各做一次，只清自己这一路）
在 F:\openidea 下执行 python tools/kill_test_procs.py --lane <LANE>，退出码非 0 表示还有残留，把输出报上来。
不要自己另写清理命令，不要按进程名杀任何东西（系统有个叫 Registry 的进程不能碰），不要 pkill redis-server，不要执行 wsl --shutdown。

## 运行
在 F:\openidea 下执行（可能 30-60 分钟；放后台、定期轮询日志，不要中途打断或重启）：
python tools/parallel_matrix.py --lane <LANE> --jobs 4 --windows-redis-binary <WINDOWS_REDIS> --artifacts <ARTIFACTS>/par-<LANE>
它会<MODE_NOTE>，跑 test_matrix.py（内部 run_regressions.py --case all）。总日志在 artifacts 目录的 <LANE>.log，结果在 parallel.json，子目录 <LANE> 里有 test_matrix 报告和各用例日志。环境问题导致失败就记录原因，不要改代码绕过。

## 回报
1. 退出码、耗时；
2. 每个用例：通过 / 失败 / 环境阻塞（用报告里的用例名）；
3. 失败用例的日志关键行（bad=、mismatch=、rpc_fail=、超时、断言、崩溃、服务端报错）及日志路径；
4. 前后清理是否完成、有无残留。
只报告看到的证据，不要猜。
````

本机当前取值：`<WINDOWS_REDIS>` 为 `C:/Users/jiangxueyang/tools/redis-8.10.3/Redis-8.10.3-Windows-x64-msys2/redis-server.exe`，`<ARTIFACTS>` 为 `C:/Users/jiangxueyang/openidea-test-results`。四路都回报后，主对话汇总逐例结果；全部通过才算这一轮测完。

## 8. 2026-11-04 验证记录

- Windows：MSVC 19.41.34123、SDK 10.0.26100，原生 x64 Release，MT/ST 各 18 项通过。
- Linux：WSL GCC 13.1，MT/ST 各 18 项通过；两模式前跳/后跳故障和纯协程 ASan 均通过。响应结果顺序修复后又跑完两模式全量及 ASan。
- 最终 Windows MT 证据在宿主 `~/openidea-test-results/final-20261104/windows-final-mt`；ST 在 `windows-fixed/st`。后者同轮的旧 MT 失败不能计为通过，已由修复后 MT 完整覆盖。
- 最终 Linux 证据在 WSL `~/openidea-test-results/final-20261104-response-fix`；校时证据在宿主 `~/openidea-test-results/final-20261104/linux`。
- 最终四组逐例结果汇总在宿主 `~/openidea-test-results/final-20261104/verified.json`，共 72 项。日志与大文件不入库。
- 本轮修复 Windows 分支变量重命名遗漏、系统 min/max 宏干扰、RPC 解码和响应移动的顺序，以及 Windows 到 WSL 的路径传递；相关失败日志保留在原结果目录。
- 早期 Linux MT 压测曾返回 291 次 OI_ERR_BUSY，独立复测及后续完整矩阵均通过。开放式百万请求压力在积压时可以触发容量保护，不能把重试通过等同于任何负载都不会限流。回声客户端此后改为固定数量的工作协程，在途数不超过 `inflight`。

## 9. 2026-11-05 复核验证记录

- Windows 原生 MSVC 19.41.34123 / SDK 10.0.26100 和 WSL GCC 13.1，MT/ST 各 18 项全部通过，共 72 项；包括独立调度的单次请求争抢等待检查。四路及主工作目录均已恢复 MT、关闭测试注入并成功构建。
- Windows Redis 使用本节前述 8.10.3 便携包，独立探针确认大块 ECHO、BLPOP 后 PING 正常，运行时内存上限 1536MiB、关闭持久化，退出后无残留。
- 四路完成后追加的 Python 清理边界修复，另在 Windows / WSL 各跑 28 项定向测试，全部通过，包含真实 Redis 端口占用、CIM 快照 PID 复用、Linux 无权检查、PID 文件写入失败收尾。清理属于纯 Python 进程管理逻辑，不依赖 C++ ST/MT；没有将这 28 项冒充追加修复后的四路全量回归。
- 最新 Linux 进程身份登记逻辑另用现成 MT 产物跑 Hold，`hold done bad=0`；真实 Windows 子进程创建时间比对及同句柄终止通过。最终两端清理返回 0，无残留报告。
- 本机证据根目录为 `~/openidea-test-results/review-2085438`：四路位于 `par-<平台>-<模式>`，追加脚本测试及主目录恢复状态位于 `final-tools`，Linux 补测位于 `final-tools-linux-hold`。大体积日志数据由用例清理，仅保留文本证据。
