# 跨平台全量测试操作规程

面向维护者与 AI：按本文件执行，不要另写客户端，不要跳过失败，不要把只编译、WSL 运行或强杀退出算成 Windows 原生测试通过。

## 1. 范围与通过条件

- Windows 原生 MSVC x64 Release、Linux WSL GCC 13，各运行多线程 MT 和单线程 ST，共四组。
- ST/MT 只切换 ipcclient、ipcserver；logd 固定多线程，redisserver/registry 按各自配置单线程。
- `all` 包含 21 项：db、echo、log、log-backpressure、redis、redis-limit、redis-shutdown、redis-queue-timeout、rpc-ingress、redis-backpressure-bytes、redis-backpressure-count、redis-fairness、hold、rpc-admission-count、rpc-admission-bytes、reset、disk-full、registry、udp、multi、web。
- Linux 可额外运行 clock-forward、clock-back，以及纯协程 ASan。校时故障只影响测试子进程，绝不调整宿主机时钟。
- 必须全部返回 0；不能用删除断言、扩大超时、提高容量或重试掩盖失败。偶发失败保留日志，记录复测结果及根因是否确认。
- 本次源码正在变化时不要声称覆盖了最新版本；核对结果目录的 environment.json、Git HEAD、diff 摘要后再报告。

`db` 使用公共 fake 驱动覆盖数据库客户端，不依赖真实 MySQL，已列入 `--case all`。`mysql` 是显式运行的独立用例，不包含在 `all`：需启用 MySQL 驱动并提供可用的客户端库、测试数据库和测试环境配置；缺少依赖或配置时记录环境阻塞，不能算通过。新增用例以实际回归结果为准。

阻塞函数回归由 HoldMgr 覆盖：128 个后台工作及 64 个共享等待者，核对结果和单次执行。阻塞池按类型隔离，Default 默认 4 个线程、1024 个排队项，并保留 1024 个在途工作的兼容限制；其他类型须显式配置，各自的队列上限独立。容量检查应区分在途工作与排队项，并覆盖超限拒绝、启动后配置拒绝及停止时排空已接纳工作。新增检查以实际矩阵结果为准，不代表四种构建模式已经通过。

输出引用 RPC 的日常压力由 EchoMgr 使用 `Call(req, res)` 覆盖。HoldMgr 同时验证输出任务的请求保活、16 个等待者只提交一次、重复等待不重写响应、固定会话、业务错误保留原响应、超时后迟到回包不覆盖新响应，以及命名/分区调用。正常 Hold 必须输出 `hold output rpc shared waiters=16 submissions=1 bad=0`、`hold output rpc direct pinned error late bad=0`、`hold output rpc named any zone shared=8 bad=0`；它们已纳入脚本断言。底层单次等待器的重复等待和异常注入用例仍保留，与输出 Task 可重复等待的语义分开验证。

路由与请求生命周期回归也集成在正常 Hold 中：容量策略隔离与真实大包收发、持有租约时不回收/释放后连接状态销毁及重新获取、注册查询限流和超时清空后的恢复、查询与回包共用端到端预算，以及保留已完成任务时请求资源仍被销毁。路由管理器固定为常驻单例，测试同时断言不同策略的连接共用管理器。脚本只在临时配置缩短池空闲时限、降低查询容量；通过暂停本次启动的 registry 进程制造有连接无回包和慢查询，finally 中恢复暂停状态再清理。应用请求仍从 ipcclient 发出，不新增测试客户端。

正常 Hold 还覆盖同一池连接断线后连续四次按需重连、出站租约释放时反向 RPC 挂起及成功回包、批量指定分区在整体超时/限流/非法分区时仍返回等长结果，以及 4096 条不同剩余时长的绝对截止时间归桶、乱序短超时不提前触发、极大时长不溢出。脚本强制检查对应标记，反向回包还核对服务端日志。矩阵同时执行禁止多继承扫描及检查器用例；安全测试自动使用矩阵指定的原生 Redis 可执行文件。

## 2. 环境预检

仓库路径可以包含空格。以下示例在仓库根目录执行，示例中的用户、构建目录和 Redis 路径按实际环境传参。

Windows：Python 3.10+、Visual Studio 2022 的“使用 C++ 的桌面开发”（MSVC v143 x64、MSBuild、Windows SDK）、CMake。仅有 cl.exe 不够，还要有 MSVC include/lib 与 SDK include/lib。脚本会寻找 PATH 中的 CMake，也能找到 VS 附带的 CMake。

Linux：优先用仓库根目录 `.local/linux-hosts.json` 里的远程机器。该文件只留在本机，不进仓库，记录每台机器的 IP、用户名、核数和职责。4 核机器编译 Release 并跑多线程，2 核机器跑单线程，登录只用公钥，不把密码写进代码或命令。文件不存在，或缺少 IP、用户名时，先问用户。用户选择不提供，就改用 WSL，直到用户再次明确说用远程机器。从 Windows 登上去跑的步骤见 `.cursor/skills/linux-remote-test/SKILL.md`。WSL 和远程都要有 Python 3.8+、cmake、gcc-13/g++-13、make、已编译的 Redis 服务端。源码编译 Redis 应使用持久外部依赖目录（例如 `~/opt/redis-7.4.12`），通过 `/usr/local/bin/redis-server` 安装入口复用；默认路径为该入口。禁止把工具装到 `/tmp` 或测试临时目录，停止测试进程时不删除工具安装或源码。路径不存在必须提供 `--redis-binary`，脚本不会下载不明 Redis 二进制。

测试占用本机端口 19995、17981、16379、16380、17990、17992、17993，另有 UDP 17983。测试 Redis 用 16380，不用 6379：Windows 动态端口段可能被 Hyper-V/WinNAT 保留，本机已有的 6379 Redis 也不会和测试冲突。复制出的 `redisserver.toml`、`registry.toml` 由脚本改到 16380，仓库里的配置仍是 6379。运行前脚本探测占用；不杀其他人的服务。同一份工作目录内不要并行；要并行就用 `tools/parallel_matrix.py`，它把工作目录同步成四份（Windows 在 `F:/oi-par`，Linux 在 WSL 的 `/root/oi-par`），Windows MT/ST、Linux MT/ST 各用一份，`--port-offset` 依次为 0、1000、2000、3000，所有测试端口整体平移，互不抢占：

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

本轮下载文件的 SHA256 为 `af16df340525b431e9ec8dabfd625d5e7a233f82805b5418819904ea909c447f`，用于复现文件身份，不代表发布方签名验证。探针仅操作自己启动的实例：核对启动日志、随机密码认证，并按进程句柄关闭；端口已占用就失败，不发送针对该端口的 `SHUTDOWN`。WSL 探针可用 `--wsl --wsl-user jxy --wsl-binary /usr/local/bin/redis-server`。

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
python tools/test_matrix.py --platform both --wsl-user jxy --linux-build-dir /home/jxy/openidea-build-gcc13 --redis-binary /usr/local/bin/redis-server --windows-redis-binary C:/tools/redis/redis-server.exe --artifacts C:/Users/Admin/openidea-test-results/manual-run
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

排查 Hold 的 Redis 迟到回包时，可对本次测试启动的 Redis 开启只读 MONITOR：

```sh
python3 tools/run_regressions.py --case hold --redis-timeline --port-offset 2000 --artifacts /path/to/results
```

`hold/redis-timeline.out` 只保留 PING 和测试用 BLPOP 的 Redis 执行时间及上游连接地址；结合 `hold/ipcclient.out` 的超时和耗时检查。MONITOR 记录执行命令，不能证明回包已送达客户端；定位丢包或错配仍需采集代理和客户端收发时间。端口偏移须与该路一致，测试前后按第 7 节清理对应路。

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
- udp：ipcserver 的 `[IpcUdp]` 和 ipcclient 的 UdpMgr，`udp done bad=0`。覆盖系统分配端口、同地址 `OpenPeer` 去重、对端地址、1000 次 RPC、1000 条单向消息不丢不重、60000 字节单个数据报、坏包不断会话（服务端会话数仍为 1）、客户端 1.5 秒空闲关会话后重开新会话。gameserver 分区的 UDP 口改为 0，不和主服抢。
- multi：ipcclient 的 MultiMgr，`multi done bad=0`。`[IpcMultiFixed]` 不配 service，写死地址只连一台，实例号 0，没连上就发的调用连上后补发。`[IpcMulti]` 配 service=peerserver，脚本用 17992、17993 起两台同名全局 ipcserver，实例号 1、2，客户端起来前都已登记。覆盖每台各一条连接和建连回调、按实例号调用打到对应服务端、名单外实例号回 CONNECT、CallAny 两台都命中、CallAll 两个结果按实例号排列、服务端顺着连接反推（请求和 NotifyPeer 各触发一次，核对推送进的是这台的会话）、写死地址 UseAddress 换到 2 号再换回（配了 service 的拒绝，地址没变不断开，IsConnect 为真）。随后脚本杀掉 2 号：断开回调触发，注册表撤掉后名单只剩 1，调 2 回 CONNECT，1 照常；再重启 2 号：名单恢复，自动重连，调 2 成功。不起 redisserver。
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

矩阵启动时先使旧汇总失效；只有本次所有步骤完成、恢复 MT 并关闭测试注入且重新构建成功，`summary.json` 才能标记 `passed: true`。中断、配置失败、恢复异常都算未通过，分别检查 `completed`、`restored` 和异常字段；不得把恢复构建成功当作测试通过。`tools/test_matrix_outcomes.py` 用模拟子进程覆盖这些异常分支，矩阵自动执行。

进程收尾会逐一尝试正常退出；控制台通知失败或超时后，仅强制结束当前用例持有的进程，并继续回收其他进程、关闭输出和保留诊断日志。强制回收仍记为失败，不算正常退出；如果用例本身已有异常，保留原始异常，同时输出清理错误。正常 Hold 回归还会在选路后、发送前切换订阅管理器的分区，检查单次与批量目标不变，输出 `hold route target stability bad=0`。

交给下一位 AI 时必须列出：实际测试的平台/编译器/线程模式、结果目录、失败或阻塞项、是否恢复 MT、是否关闭测试注入、是否清理临时数据。缺 SDK、缺 Redis、编译失败或操作被拦截，都只能记录为阻塞/失败；不能写“全量通过”。

## 7. 四路并行的 subagent 提示词范本

进程清理统一用 `tools/kill_test_procs.py`：按本仓库或本路副本的完整路径，以及临时目录中的 `.oi-test-owner.json` 归属标记匹配。标记记录仓库、端口偏移、PID 及创建标识（Windows FILETIME、Linux `/proc` starttime）。Windows 的路径归属还须核对 CIM 快照创建时间，并通过同一进程句柄验证身份、执行终止。端口、目录前缀和通用脚本名都不能单独证明进程归属。WSL 保留 6379，不执行 `wsl --shutdown`；已知属于测试的进程无法检查、清理错误或有残留，必须返回非零。`--dry-run` 只列不杀。

换机器时，清理和矩阵必须使用相同的 `--windows-root`、`--linux-root`、`--wsl-user`；清理旧的无标记实例时，先人工核实归属，不能退回按端口杀进程。脚本安全回归为 `python tools/test_test_safety.py`，矩阵会自动运行。

先由主对话做一次全局清理 `python tools/kill_test_procs.py`，Cursor 里 subagent 固定用 `grok-4.7-high`（Grok 4.7）。Codex 里固定用 Terra，不用 Astra。不用每轮再问；只有用户要求更换时再确认。四个 subagent 同时在后台派发，每个一路，按下表替换范本里的占位符。

| `<LANE>` | `<OFFSET>` | `<OTHERS>` | `<MODE_NOTE>` |
| --- | --- | --- | --- |
| windows-mt | 0 | windows-st / linux-mt / linux-st | 在 F:\oi-par\windows-mt 编译 MT |
| windows-st | 1000 | windows-mt / linux-mt / linux-st | 在 F:\oi-par\windows-st 编译 ST，最后恢复 MT |
| linux-mt | 2000 | windows-mt / windows-st / linux-st | 在 WSL /root/oi-par/linux-mt 编译 MT，Redis 用 /usr/local/bin/redis-server |
| linux-st | 3000 | windows-mt / windows-st / linux-mt | 在 WSL /root/oi-par/linux-st 编译 ST，最后恢复 MT，Redis 用 /usr/local/bin/redis-server |

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

## 10. 2026-11-05 路由与测试收尾修复验证

- Windows 原生 MSVC 与 WSL GCC13，MT/ST 各 18 项运行用例通过，共 72 项；四路 Hold 均确认 `hold route target stability bad=0`、`hold done bad=0`。
- 脚本定向测试每个平台 54 项通过：最初因未设置环境变量而跳过的真实 Redis 端口冲突探针，已分别指定原生 Redis 路径补跑通过。工具测试覆盖控制台通知失败后继续清理、保留原异常，以及中断/恢复失败不误报成功。
- 本机证据根目录为 `~/openidea-test-results/fix-review-20261104`，最终四路在 `par-<平台>-<模式>-fixed`，脚本证据在 `tool-tests`，主目录构建与静态检查在 `main-restore`。首轮新增用例的模板参数编译错误已修正；原失败目录保留，不计为通过。
- 格式检查 136 个文件通过，命名基础测试 5 项通过。libclang 命名扫描仍返回 1：现有 `hold_mgr.cpp` 的 `fnContend` lambda 被识别为 `auto` 而要求 `st` 前缀，按可调用对象应使用 `fn`，属于既有误报；没有将该扫描记为通过。
- 主目录及四路副本均恢复 MT、关闭测试注入并构建成功；最终全局清理返回 0，无残留报告。临时日志数据已由测试清理，仅保留诊断输出。

## 11. 2026-11-05 RPC 输出参数接口验证

- Windows 原生 MSVC 19.41.34123 / SDK 10.0.26100 与 WSL GCC13，MT/ST 各 18 项通过，共 72 项。四路回声均通过新的输出参数接口完成 100 万次请求，`mismatch=0`、`rpc_fail=0`。
- 四路 Hold 均确认输出任务 16 个等待者只提交一次、命名路由 8 个共享等待者，以及完成后重复等待、请求生命周期、业务错误、超时迟到回包不覆盖响应等检查通过；三个新增标记及 `hold done bad=0` 均满足。
- 每路 Python 工具检查 58 项通过、1 项跳过：未设置 `OI_SAFETY_REDIS_BINARY`，本轮没有执行可选的真实 Redis 端口冲突探针。应用层 Redis 收发用例已全部通过，不能将该工具探针记为通过。
- 主工作目录 Windows / Linux 构建成功，格式检查 136 个文件通过；主目录及四路副本均恢复 MT、关闭测试注入。最终全局清理返回 0，无残留报告。
- 本机证据根目录为 `~/openidea-test-results/rpc-output-20261105`，四路在 `par-<平台>-<模式>`，主目录验证在 `preflight` 和 `main-windows`，汇总为 `verified.json`。临时日志数据已清理，仅保留诊断证据。

## 12. 2026-11-05 五项修复验证

- 编译与测试全部交给 Terra。Windows 原生 MSVC 与 WSL GCC13，MT/ST 各 18 项通过，共 72 项；四路均完成百万次 Echo，`mismatch=0`、`rpc_fail=0`。
- 新增 Hold 检查覆盖：已完成输出任务释放请求资源、路由配置隔离及空闲回收后重连、路由池容量保护、注册查询限流和超时恢复、命名 RPC 端到端超时预算。五项标记及 `hold done bad=0` 均通过，保留完成任务重复等待时 20 份请求资源全部释放。
- 每路测试清理安全检查 41 项通过、无跳过，包含指定本平台 Redis 可执行文件后的真实端口占用检查。测试日志数据由用例清理，单例最大落盘约 1.07GB，未达到 2GiB 停止阈值。
- 首轮发现会话构造新增的管理器保活逻辑解引用空指针，已恢复空管理器兼容；原测试保留，修复后四路全部重跑。旧失败目录保留，不计入通过结果。
- 主工作目录 Windows/Linux 构建成功，格式检查 136 个文件、命名基础测试 5 项通过；本轮未运行 libclang 全量命名扫描。主目录及四路副本均恢复 MT、关闭测试注入。最终全局清理退出 0，无残留报告。
- 本机证据根目录为 `C:/Users/Admin/openidea-test-results/five-fixes-20261105`，最终四路位于 `par-<平台>-<模式>-fixed`，主目录构建证据在 `main-final`，逐例汇总为 `verified.json`。该汇总只使用修复后四路结果。

## 13. 2026-11-05 常驻路由管理器与禁止多继承验证

- 编译与测试全部交给 Terra。Windows 原生 MSVC 与 WSL GCC13，单、多线程各 18 项全部通过，共 72 项；新增重连、入站反向 RPC 保活、批量失败结果数量、绝对截止时间桶检查均通过，服务端确认反向回包成功。
- 自有 C++ 禁止多继承扫描覆盖 147 个文件，检查器自身 4 项通过，已加入完整矩阵。每路进程安全检查 41 项通过、无跳过。
- 主目录 Windows/Linux 构建通过，格式检查 137 个文件、命名基础测试 5 项、矩阵结果测试 8 项通过。Linux 单、多线程协程共享等待 ASan 专项通过；仅专项探针启用 ASan，不代表整个基础库经过 ASan 构建。
- 主目录与四路副本均恢复多线程并关闭测试注入。四路前后清理及最终全局清理均退出 0，无残留报告；临时日志数据已由用例清理，最大单例约 1.07GB，低于 2GiB 阈值。
- 本机证据在 `C:/Users/Admin/openidea-test-results/route-review-20261105`，最终四路在 `final/par-<平台>-<模式>`，主目录检查在 `main-final`，汇总为 `verified.json`。旧动态管理器方案的预检失败保留在 `preflight`，不计为通过。
- Linux 副本的 Git diff 哈希与 Windows 不同；收尾时对两个 Linux 副本与主目录逐字节核对，754 个路径均一致，证据在 `final/source-byte-comparison.json`。这是收尾内容核对，不代替测试开始时的独立快照证明。测试完成后仅追加本节文档。


## 14. 断连与背压边界回归

- Hold 的 `hold pooled reconnect bad=0` 必须在主动断开后不 yield、不等待清理，立即向同一连接发 RPC，验证重连排队。
- `hold route backpressure status bad=0` 检查已连接发送及建连后的缓存补发均准确返回 BUSY，背压不换会话，随后的小请求仍可成功。发送预算专项同时检查满载返回 BUSY、关闭返回 CONNECT。
- `hold disconnect drain calls=4096 bad=0` 跨 64 个分片及固定大小批次清理 4096 项，首个完成回调注入 bad_alloc，验证其他回调继续、结果全部为 CONNECT、保护计数归零、等待索引恢复基线。清理本身不动态扩容；该注入发生在完成回调，不是全局内存耗尽模拟。
- 以上均由常规 `--case all` 的 Hold 触发，并由回归脚本断言。


## 15. Redis 类型化单 key 命令回归

常规 RedisMgr 完整性测试及压力测试使用标准命令接口，原生 CallRedis 保留给 PING、Lua 和协议/状态隔离探针。强制标记为 `redis typed commands string rpcdata zset bad=0`，覆盖二进制 key/value、空字符串/缺失值、WRONGTYPE、rpcdata 正常/损坏、集合中途解码失败不覆盖原输出、hash rpcdata、排序集合正反序/分数/更新计数、NaN 拒绝和共享 Task 重复等待。同步压力线程同时覆盖 string 与 rpcdata 存取。所有标准命令只使用一个 key，不以批量多 key 指令代替逐条调用。


Redis 类型回归还必须确认 `redis typed collections string rpcdata bad=0`、`redis typed native ping lua scalar array rpcdata bad=0` 和两个 `redis typed sync id=0/1 bad=0` 标记。覆盖 string/hash/list/set/zset 常用读写/计数/删除，空值与错误类型，Lua 返回整数/布尔/字符串/数字字符串/数组/rpcdata、Lua 错误与 nil、整数溢出和结果形状不匹配；所有失败保持原输出。同步线程覆盖 PING、Lua、字符串、rpcdata、列表、hash、set、zset 与分数。Lua 脚本若需访问存储，只显式声明一个 key；不新增隐式多 key 用例。

新增五类单 key 命令的 RedisMgr 回归要求 `redis expanded five types bad=0`，覆盖条件写、增减、批量与空参数、二进制字符串、rpcdata、带 nil 的 HMGET/ZMSCORE、正反范围及弹出删除。HSCAN、SSCAN、ZSCAN 累积到游标为零，不假定 COUNT 指定返回数量；同步压力线程也检查 optional 及三种扫描的 rpcdata 解码。

Redis 断言按每条命令分别等待、检查返回值和输出，失败日志包含检查行号。GCC13 下复合多 await 条件复用输出曾造成 6 个集合断言失败，布尔位数组代理曾在读取时崩溃；不要通过减少断言或跳过 Linux 消除此类失败。成员批量判断使用 0/1 整数数组，原生类型化调用也拒绝 vector<bool>，布尔标量仍支持。

## 16. 2026-11-05 Redis 五类命令最终验证

- Terra 执行 Windows 原生 MSVC、WSL GCC13 的单/多线程全量矩阵，每路 18 项全部 passed，共 72 项，没有跳过或失败。随后仅调整过期测试的多 await 表达式，生产代码不变；四路同步最新源码、重新编译并各补跑 Redis 用例，全部通过。最新 Redis 结果以补跑为准，不把旧失败版本计入最终通过数。
- 每路最新 Redis 日志同时核对 typed commands、collections、native PING/Lua、expanded five types、sync id 0/1、毫秒过期、integrity 和 stress；所有 bad 均为 0。全量 Hold 的立即重连、BUSY 背压回执和 4096 等待者断线排空也通过。
- 主目录 Windows/Linux 全目标构建及格式检查通过；七个新 Redis 头分别独立包含的 Windows 编译通过。主目录和四个副本均恢复 MT、关闭测试钩子并重建；最终全局进程清理与残留核查均退出 0，用户 6379 Redis 未处理。临时大日志由用例清理。
- 最后逐字节核对主目录与四个隔离副本的 202 个自有源码、头、CMake 与脚本文件，全部一致。文档、三方源码和构建产物不在该核对范围内；本节在测试后追加。
- 本机证据根目录为 `C:/Users/Admin/openidea-test-results/redis-complete-20261105`：最终全量在 `final/par-<平台>-<模式>`，最终 Redis 补跑在 `final/expiry-par-<平台>-<模式>`，主目录最后构建在 `final/main-expiry-rebuild`，汇总为 `final/verified.json`，源码比较为 `final/source-byte-comparison.json`。段错误、六项旧集合断言失败和 GCC13 独立最小复现均保留在非 final 目录，不能视为通过证据。


## 17. 原始异常日志回归

`redis`、`rpc-ingress`、`hold` 三项增加 `diagnostic original errors persisted <case> PASS` 标记。测试通过正常退出排空日志服后检查真实 error 文件，不能只检查客户端打印：

- Redis 检查 Lua 原始错误和 WRONGTYPE 的终端记录，并检查 Lua 错误及请求/会话/命令信息已落盘；原有标准返回码和输出不变断言继续执行。
- RPC 入站测试发送合法帧包裹损坏的字段头，验证失败回执调用号仍为 424242，并确认解包阶段、会话、协议号、调用号和 ParserException 已落盘。
- Hold 触发 handler 异常和响应序列化异常，确认两种原始异常消息与调用上下文已落盘；失败不覆盖输出，后续请求正常恢复。另覆盖诊断文本的换行、NUL、反斜杠转义、百分号原样保留和 4096 字节截断边界。

上述检查复用现有模块与开关，纳入 `--case all`，Windows/Linux 单、多线程均需执行。它们验证正常日志管道可落盘，不代表日志服断连、磁盘损坏或进程被强杀时仍保证日志持久化。

### 2026-11-05 验证结果

Terra 执行 Windows 原生 MSVC、WSL GCC13 单/多线程矩阵，最终每路 18 项通过，共 72 项；四路三个原始错误落盘标记均通过，副本均恢复 MT 并关闭测试注入。本轮改动涉及的 9 个源码/脚本文件在四份测试副本与主目录逐字节一致。

证据根目录为 `C:/Users/Admin/openidea-test-results/error-diagnostics-20261105`。Windows MT 最终使用 `final-retry/par-windows-mt`，另外三路使用 `final/par-<平台>-<模式>`，逐例汇总为 `final/verified.json`。Windows MT 前一轮业务检查通过，但 Web 临时副本删除报 WinError 5，整轮按失败保留；同源码全量复测通过，不将旧失败报告改为成功。

Linux 首轮使用了错误的 `/usr/bin/redis-server` 路径，失败记录保留；原有 Redis 实际仍在 `/tmp/redis-7.4.12`，没有被删除。本轮直接复制已有安装到 `/home/jxy/opt/redis-7.4.12`，建立 `/usr/local/bin/redis-server` 和 `redis-cli` 入口，无需重新下载；原副本保留，两份服务端二进制 SHA256 一致。后续复测使用持久入口，清理仅停止本次进程及移除临时日志/数据，保留第三方安装。

主目录 Windows/Linux MT 生产构建均成功，测试钩子关闭；最终全局清理和 dry-run 核查均退出 0，无残留报告。两端 Redis 持久安装保留，测试临时大日志由用例清理。主目录构建日志在 `final/main-windows`、`final/main-linux`，最终清理证据在 `final/cleanup.json`。


## 18. 直连背压、客户端解包与 Redis 上游断连回归

普通 Redis 用例另启用临时 `Regression.redis_send_budget=true`，只将该客户端的 `max_send_queue_bytes` 设为 1MiB。循环 8 次在先发小请求之后分别通过原生回执、类型化输出接口拒绝 2MiB 请求，再发送小请求；检查 16 次拒绝均为 BUSY、失败不覆盖输出、16 次小响应内容正确、连接未重建、等待项清零。通过标记为 `redis send budget rejected=16 recovered=16 pending=0 bad=0`，并检查 `stage=send` 的容量错误原因已由日志服落盘。

`redis-shutdown` 临时配置的请求超时为 10 秒，避免 Windows 启动控制台通知辅助进程时先触发原来的 300ms 请求超时；仍要求停服唤醒为 Disconnected、停止后新调用立即失败、等待项清零，并保留通知后的 3 秒退出时限。短超时正确性由独立 `redis-queue-timeout` 用例验证，不把 Timeout 当作停服成功。

- Hold 新增 `hold direct backpressure queued=... direct=... pinned=... closed=... bad=0`：常驻测试管理器使用 256 字节真实会话发送预算，超过预算的请求分别在建连前排队、已连接直发和显式 sid 路径返回 BUSY，输出保持原值；小请求正常完成，关闭后的显式 sid 调用返回 CONNECT。
- `Regression.encoding_failure` 的现有服务端注入同时支持合法外层回执中的损坏字段体。Hold 检查客户端返回 FAIL、输出不被半包覆盖、后续正常请求成功，并输出 `hold client malformed response sid=... bad=0`。驱动核对日志服 error 文件中的 `decode_response`、实际来源 sid、调用号、协议号和原始 ParserException。
- 普通 Redis 用例的临时配置启用 `Regression.redis_upstream_disconnect`。RedisMgr 启动阻塞请求并通知既有驱动，驱动仅通过自己持有的进程句柄终止本次 Redis，然后复用持久可执行文件重启；不按进程名或端口杀服务、不删除安装。等待请求失败且输出不变，代理自动重连后 PING 返回 PONG，输出 `redis upstream disconnect recovery bad=0`。驱动还核对日志服落盘记录的上游地址、非零错误状态和原始 hiredis 原因。

全部纳入现有 `--case all` 的 Hold / Redis，用已有模块及开关复测，不新增独立客户端。Windows/Linux 单、多线程各跑完整矩阵；故障注入只影响本次独立 Redis，不操作用户 6379 服务。

### 2026-11-05 三项修复验证结果

Terra 执行 Windows 原生 MSVC、WSL GCC13 单/多线程完整矩阵，每路 18 项全部通过，共 72 项。四路直接发送/建连排队背压、损坏响应、Redis 上游断连恢复及对应原始错误落盘检查均通过；测试副本均恢复 MT 并关闭测试钩子。本轮 8 个变更源码/脚本文件在主目录与四份测试副本逐字节一致。

证据根目录为 `C:/Users/Admin/openidea-test-results/three-review-fixes-20261105`，最终结果位于 `final-fixed/par-<平台>-<模式>`，汇总核验为 `final-fixed/verified.json`。首轮失败保留在 `final`：旧 Hold 断言仍期待通用 FAIL，而修复后无效显式 sid 正确返回 CONNECT；Redis 故障注入已终止的旧进程仍被驱动存活检查误判。已同步错误码断言，并在确认旧进程退出后将其移出存活检查，再运行完整四路，没有忽略失败或降低新实例检查。

主目录 Windows/Linux MT 生产构建均退出 0，测试钩子关闭，日志在 `final-fixed/main-windows`、`final-fixed/main-linux`。最终全局清理与 dry-run 均退出 0，无残留报告，记录在 `final-fixed/cleanup.json`；持久 Redis 安装保留。

### 2026-11-05 Redis 客户端发送错误码补充验证

证据根目录为 `C:/Users/Admin/openidea-test-results/redis-send-budget-20261105`。最终 Windows MT、Linux MT/ST 使用 `verified/par-<平台>-<模式>`；Windows ST 使用同源码独立复测的 `isolated/par-windows-st`。每路 18 项通过，共 72 项；四路发送预算拒绝、失败输出保持、后续响应配对、等待项清零及发送失败原始原因落盘均通过。逐项汇总为 `verified/verified.json`，主目录与四份测试副本的 9 个变更源码/脚本文件 SHA256 一致。

首轮 GCC13 在新增测试的协程临时参数表达式上发生内部编译错误；将参数拆为具名 vector 后，两路 GCC13 完整编译回归通过。Windows ST 首轮停服用例先触发 300ms 请求超时，已按上文调整测试时限，仍严格要求 Disconnected。上述失败记录保留。

Windows ST 的 `verified` 轮另有两项在日志服务监听就绪检查中超时，服务输出为空，控制台通知辅助进程退出 120；不能据此断定根因。同代码独立完整复测 18 项通过，未复现，旧失败报告仍保留，不把该轮改写为成功。

两端主目录 MT Release 生产构建均退出 0、测试钩子关闭，日志在 `verified/main-windows`、`verified/main-linux`。最终全局清理和 dry-run 均退出 0、无残留报告，记录在 `verified/cleanup.json`；Windows/WSL 的持久 Redis 安装保留。

## 阻塞工作管道回归

Hold 通过 `uv_pipe` 创建阻塞管道，在后台普通同步函数中调用 `uv_fs_read`；后台回调只执行普通同步代码，不调用 `co_await`、`StartTask` 或其他框架协程接口。空管道读开始后，前台协程计时 200 毫秒并完成真实 RPC，确认写入前读尚未结束；随后向管道写入 `pipe-ok`，等待读取结束并检查内容与返回码。异常和写入失败路径也要释放读端并等待后台结束。另以固定种子 20261110 在前台生成 1–20 毫秒延迟，128 个后台任务分别同步等待并返回不同值，按提交顺序核对 WhenAll 结果，不要求实际完成顺序必须变化。驱动强制检查 `hold blocking delay seed=20261110 min_ms=1 max_ms=20`、`hold blocking read started=1 timer=1 rpc=1 early=0 content=1 bad=0`，以及 `hold blocking tasks=128 waiters=64 runs=1 bad=0`。
