# 跨平台全量测试操作规程

面向维护者与 AI：按本文件执行，不要另写客户端，不要跳过失败，不要把只编译、WSL 运行或强杀退出算成 Windows 原生测试通过。最近一次结果写在 [lasttesting.md](lasttesting.md)，不要写进本文件。

## 1. 范围与通过条件

- Windows 原生 MSVC x64 Release、Linux WSL GCC 13，各运行多线程 MT 和单线程 ST，共四组。
- ST/MT 只切换 ipcclient、ipcserver；logd 固定多线程，redisserver/registry 按各自配置单线程。
- `all` 包含 20 项：echo、log、log-backpressure、redis、redis-limit、redis-shutdown、redis-queue-timeout、rpc-ingress、redis-backpressure-bytes、redis-backpressure-count、redis-fairness、hold、rpc-admission-count、rpc-admission-bytes、reset、disk-full、registry、udp、multi、web。
- Linux 可额外运行 clock-forward、clock-back，以及纯协程 ASan。校时故障只影响测试子进程，绝不调整宿主机时钟。
- 必须全部返回 0；不能用删除断言、扩大超时、提高容量或重试掩盖失败。偶发失败保留日志，记录复测结果及根因是否确认。
- 本次源码正在变化时不要声称覆盖了最新版本；核对结果目录的 environment.json、Git HEAD、diff 摘要后再报告。

协程压力和序列化压力不包含在 `all`。一个月测一次，同一台机器上一次只跑一个，不要并排，它们会占满 CPU。协程压力是 `python3 tools/run_regressions.py --case coro`，开关是 `ipcclient` 的 `[Test] coro`，量级在 `[TestCoro]`，默认 10 万条同时挂起、100 万条创建后立即结束。序列化压力是 `python3 tools/gen_dispatch_bench.py array` 或 `switch`，不进回归进程。结果写进 [lasttesting.md](lasttesting.md)，不要写进本文件。

数据库只有 `mysql` 一个用例，显式运行，不包含在 `all`。标准入口是矩阵参数 `--mysqld <mysqld>`（`tools/test_matrix.py`，或 `tools/parallel_matrix.py` 的 Linux 路）：每种线程模式编译完成后先跑 `<mode>-mysql`，再跑全量，不额外编译。非标准安装用 `--define OI_MYSQL_INCLUDE_DIR=... --define OI_MYSQL_LIBRARY=...`。单独复现仍可直接用 `tools/run_mysql_regression.py`。WSL 的 MySQL 来自 apt 包 `mysql-server-core-8.0`、`mysql-client-core-8.0`、`libmysqlclient-dev`，不起系统服务，`mysqld` 用 `/usr/sbin/mysqld`。需提供可用的客户端库；缺少依赖或配置时记录环境阻塞，不能算通过。Linux 上 `mysqld` 缺 `libaio.so.1` 时，把已有兼容库目录放进 `LD_LIBRARY_PATH`。用例先不连库检查 `DbSample` 的行映射：字段表、按成员指针取列、按列名的参数和单元格、拒绝 NULL 和坏值、生成的建表和增删改查语句逐字比对、坏表名和主键列拒绝、PB 往返和固定小端字节、JSON，输出 `mysql row mapping bad=0`；再用生成的语句建真表，整行写入读回、主键重复拒绝、按成员指针部分更新和部分读取、upsert 已有行和新行、改全部非主键列、按主键删，并用手写 SQL 注入 NULL 和坏 blob 验证读回拒绝，输出 `mysql rows bad=0`；最后是客户端本身的参数、连接复用、会话清理、事务回滚和结果上限，三项都通过才输出 `mysql done bad=0`。

阻塞函数回归由 `BlockingMgr` 覆盖：128 个后台工作及 64 个共享等待者，核对结果和单次执行。阻塞池按类型隔离，Default 默认 4 个线程、1024 个排队项，并保留 1024 个在途工作的兼容限制；其他类型须显式配置，各自的队列上限独立。容量检查应区分在途工作与排队项，并覆盖超限拒绝、启动后配置拒绝及停止时排空已接纳工作。新增检查以实际矩阵结果为准，不代表四种构建模式已经通过。

输出引用 RPC 的日常压力由 EchoMgr 使用 `Call(req, res)` 覆盖。`RpcOutputMgr` 验证输出任务的请求保活、16 个等待者只提交一次、重复等待不重写响应、固定会话、业务错误保留原响应、超时后迟到回包不覆盖新响应。`RpcServiceMgr` 验证命名和分区调用。正常 Hold 必须输出 `hold output rpc shared waiters=16 submissions=1 bad=0`、`hold output rpc direct pinned error late bad=0`、`hold output rpc named any zone shared=8 bad=0`；它们已纳入脚本断言。底层单次等待器的重复等待和异常注入用例仍保留，与输出 Task 可重复等待的语义分开验证。

正常 Hold 的二进制容器检查覆盖空和非空容器、嵌套容器与 rpcdata、连续流式对象、后续字段、固定黄金字节，以及畸形数量、截断、尾随字节和重复 key 拒绝并保持输出；同时检查数据库 blob 的格式和严格解码。脚本要求 `hold binary container bad=0`。

路由与请求生命周期回归也集成在正常 Hold 中：容量策略隔离与真实大包收发、持有租约时不回收/释放后连接状态销毁及重新获取、暂停期间已缓存路由仍可用、未缓存名字超时或在恢复后得到空名单、短暂停后未缓存名字等后台拉取，以及保留已完成任务时请求资源仍被销毁。路由管理器固定为常驻单例，测试同时断言不同策略的连接共用管理器。脚本只在临时配置缩短池空闲时限，并把 `pull_ms` 拉长，避免周期拉取打断用例；通过暂停本次启动的 registry 进程覆盖上述缓存语义，finally 中恢复暂停状态再清理。应用请求仍从 ipcclient 发出，不新增测试客户端。

暂停期间已缓存路由要求 `hold registry pause cached many=3/3 call=0 mark=831 bad=0`。未缓存名字要求 `hold registry pause missing snapshot=0 code=-2 elapsed_ms=` 小于 450 且 `mark=-731 bad=0`。恢复后空名单要求 `hold registry resume missing snapshot=1 empty=1 code=-3 bad=0`（`OI_ERR_CONNECT`）。短暂停预算要求 `hold registry budget miss code=-3 elapsed_ms=` 落在 [500, 2450) 且 `cached=0 mark=841 bad=0`。

`[Test] hold` 仍是一个开关、一次进程。`HoldMgr::Pump` 按固定顺序调用各主题 mgr，日志标记不变，不另开用例。代码在 `modules/ipcclient`：`TimerMgr`、`NestedResultMgr`、`TaskStartMgr`、`TaskShareMgr`、`BlockingMgr`、`ScheduleMgr`、`ConnectMgr`、`AddressMgr`、`IsolationMgr`、`BackpressureMgr`、`RpcOutputMgr`、`RpcAwaitMgr`、`RpcAdmissionMgr`、`RpcPendingMgr`、`RpcServiceMgr`、`RouteMgr`、`RegistryMgr`、`JsonMgr`、`BinaryMgr`、`EncodeMgr`、`HttpMgr`、`BudgetMgr`。超时、回包撞车和 Redis 迟到回包留在 `HoldMgr`。

`RouteMgr` 覆盖同一池连接断线后连续四次按需重连、出站租约释放时反向 RPC 挂起及成功回包、批量指定分区在整体超时、限流或非法分区时仍返回等长结果。`RpcPendingMgr` 覆盖 4096 条不同剩余时长的绝对截止时间归桶、乱序短超时不提前触发、极大时长不溢出。脚本强制检查对应标记，反向回包还核对服务端日志。矩阵同时执行禁止多继承扫描及检查器用例；安全测试自动使用矩阵指定的原生 Redis 可执行文件。

## 2. 环境预检

仓库路径可以包含空格。以下示例在仓库根目录执行，示例中的用户、构建目录和 Redis 路径按实际环境传参。

Windows：Python 3.10+、Visual Studio 2022 的“使用 C++ 的桌面开发”（MSVC v143 x64、MSBuild、Windows SDK）、CMake。仅有 cl.exe 不够，还要有 MSVC include/lib 与 SDK include/lib。脚本会寻找 PATH 中的 CMake，也能找到 VS 附带的 CMake。配置前检查这几项；缺 CMake、MSVC、MSBuild 或 Windows SDK 时，终端写明缺哪一项和安装步骤，并停止编译。CMake 配置或编译日志里出现找不到生成器、编译器或 v143 工具集时，同样把这段说明打到终端并追加进该步日志。

Linux：优先用仓库根目录 `.local/linux-hosts.json` 里的远程机器。该文件只留在本机，不进仓库，记录每台机器的 IP、用户名、核数和职责。4 核机器编译 Release 并跑多线程，2 核机器跑单线程，登录只用公钥，不把密码写进代码或命令。文件不存在，或缺少 IP、用户名时，先问用户。用户选择不提供，就改用 WSL，直到用户再次明确说用远程机器。从 Windows 同步源码只用 `python tools/sync_linux_remote.py --role mt|st|all`，不要手写 tar/scp；远程登录与矩阵步骤见 `.cursor/skills/linux-remote-test/SKILL.md`。WSL 和远程都要有 Python 3.8+、cmake、gcc-13/g++-13、make、已编译的 Redis 服务端。缺 CMake、gcc-13 或 g++-13 时，终端写明缺哪一个以及 apt 安装命令，并停止编译。源码编译 Redis 应使用持久外部依赖目录（例如 `~/opt/redis-7.4.12`），通过 `/usr/local/bin/redis-server` 安装入口复用；默认路径为该入口。禁止把工具装到 `/tmp` 或测试临时目录，停止测试进程时不删除工具安装或源码。路径不存在必须提供 `--redis-binary`，脚本不会下载不明 Redis 二进制。

测试占用本机端口 19995、17981、16379、16380、17990、17992、17993，另有 UDP 17983。测试 Redis 用 16380，不用 6379：Windows 动态端口段可能被 Hyper-V/WinNAT 保留，本机已有的 6379 Redis 也不会和测试冲突。复制出的 `redisserver.toml`、`registry.toml` 由脚本改到 16380，仓库里的配置仍是 6379。运行前脚本探测占用；不杀其他人的服务。同一份工作目录内不要并行；要并行就用 `tools/parallel_matrix.py`，它把工作目录同步成四份（Windows 在 `F:/oi-par`，Linux 在 WSL 的 `/root/oi-par`），Windows MT/ST、Linux MT/ST 各用一份，`--port-offset` 依次为 0、1000、2000、3000，所有测试端口整体平移，互不抢占：

```powershell
python tools/parallel_matrix.py --windows-redis-binary C:/tools/redis/redis-server.exe --artifacts C:/Users/Admin/openidea-test-results/parallel
```

`--lane windows-mt` 之类只跑其中一路，可分给多个 subagent 各跑一路。临时目录前缀带偏移（`oi-regression-p<偏移>-`），每路清理进程只匹配自己那份工作目录和自己的前缀，不要动别的路。每份各自编译、测试并恢复 MT，不碰主工作目录的 `deploy/bin`；改动跑通后主目录仍要按第 3 节编回 MT。

Windows 侧测试 Redis 用 Windows 原生 `redis-server.exe`（[redis-windows](https://github.com/redis-windows/redis-windows/releases) 的 msys2 便携包，解压即用，不装服务），`--platform both` 用 `--windows-redis-binary` 指定，直接跑 `run_regressions.py` 时用 `--redis-binary`。不要再用 `--redis-wsl`：WSL 的 localhost 转发和 NAT 内网地址在大块双向流量下都会卡死（纯 Python 收发也复现，64MB 下行只收到 3.6MB 就停住），Redis 回压、hold、registry 会假失败。怀疑链路时先用不经被测代码的探针：`tools/diag/redis_link_probe.py --native <exe>` 或 `--wsl [--host <WSL 地址>]` 测大块 ECHO 和 BLPOP 后 PING；`tools/diag/tcp_bulk.py` 测纯 TCP 下行、上行、双向各 64MiB，输出 `STALL` 即链路问题。Redis 为脚本独立启动的临时实例，内存上限 1536MiB、关闭持久化，绝不借用现有数据库。

## 3. 只编译

不要手写 `cmake`。并行度默认是本机逻辑核数。

```powershell
python tools/build.py
python tools/build.py --mode st
```

Windows 用 `build-win`、Visual Studio 17 2022、x64、Release。Linux 用 `$HOME/openidea-build-gcc13`、gcc-13。`--mode st` 只把 ipcclient/ipcserver 编成单线程，不会自动编回多线程。产物仍由最外层 `CMakeLists.txt` 放到 `deploy/bin`。

## 3.1 推荐的一键矩阵

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

本轮下载文件的 SHA256 为 `af16df340525b431e9ec8dabfd625d5e7a233f82805b5418819904ea909c447f`，用于复现文件身份，不代表发布方签名验证。探针仅操作自己启动的实例：核对启动日志、随机密码认证，并按进程句柄关闭；端口已占用就失败，不发送针对该端口的 `SHUTDOWN`。WSL 探针可用 `--wsl --wsl-user root --wsl-binary /usr/local/bin/redis-server`。

### 执行矩阵

Windows 宿主 PowerShell，同一次调用顺序运行 Windows、Linux：

```powershell
python tools/test_matrix.py --platform both --wsl-user root --windows-redis-binary C:/tools/redis/redis-server.exe --asan --clock-faults
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
python tools/test_matrix.py --platform both --wsl-user root --linux-build-dir /root/openidea-build-gcc13 --redis-binary /usr/local/bin/redis-server --windows-redis-binary C:/tools/redis/redis-server.exe --artifacts C:/Users/Admin/openidea-test-results/manual-run
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

矩阵启动时先使旧汇总失效；只有本次所有步骤完成、恢复 MT 并关闭测试注入且重新构建成功，`summary.json` 才能标记 `passed: true`。中断、配置失败、恢复异常都算未通过，分别检查 `completed`、`restored` 和异常字段；不得把恢复构建成功当作测试通过。固定只跑单线程的远程 Linux 机器用 `--mode st --no-restore`：不编回多线程，`restore_skipped` 为 true，用例全过即通过；Windows、`--platform both` 和其他模式拒绝这个开关，本机和 WSL 仍必须恢复。`tools/test_matrix_outcomes.py` 用模拟子进程覆盖这些异常分支，矩阵自动执行。

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
你在 Windows 宿主上为仓库 F:\openidea2 跑回归矩阵的其中一路：**<LANE>**（端口偏移 <OFFSET>）。另外三路（<OTHERS>）正由别的 agent 同时跑，绝不能杀或动它们的进程和目录。WSL 里用户自己的 Redis 在 6379，绝不能杀。只跑测试和报告，不要改仓库任何文件，不要 git commit/push。用中文回报。

## 清理（开始前和结束后各做一次，只清自己这一路）
在 F:\openidea2 下执行 python tools/kill_test_procs.py --lane <LANE>，退出码非 0 表示还有残留，把输出报上来。
不要自己另写清理命令，不要按进程名杀任何东西（系统有个叫 Registry 的进程不能碰），不要 pkill redis-server，不要执行 wsl --shutdown。

## 运行
在 F:\openidea2 下用 Shell 把下面这一条命令放后台跑（可能 30-60 分钟）。等待只用 AwaitShell 匹配 `^(PASS|FAIL) <LANE> seconds=`，不要自己拼后台脚本或等待文件，不要中途打断或重启。
python tools/parallel_matrix.py --lane <LANE> --jobs 4 --windows-redis-binary <WINDOWS_REDIS> --artifacts <ARTIFACTS>/par-<LANE>
Linux 两路（linux-mt、linux-st）改跑：
python tools/parallel_matrix.py --lane <LANE> --jobs 4 --windows-redis-binary <WINDOWS_REDIS> --artifacts <ARTIFACTS>/par-<LANE> --mysqld /usr/sbin/mysqld
它会<MODE_NOTE>，跑 test_matrix.py（内部 run_regressions.py --case all）。总日志在 artifacts 目录的 <LANE>.log，结果在 parallel.json，子目录 <LANE> 里有 test_matrix 报告和各用例日志。环境问题导致失败就记录原因，不要改代码绕过。

## 回报
1. 退出码、耗时；
2. 每个用例：通过 / 失败 / 环境阻塞（用报告里的用例名）；
3. 失败用例的日志关键行（bad=、mismatch=、rpc_fail=、超时、断言、崩溃、服务端报错）及日志路径；
4. 前后清理是否完成、有无残留；
5. Linux 路 mysql 三行 bad= 原文：`mysql row mapping bad=`、`mysql rows bad=`、`mysql done bad=`。
只报告看到的证据，不要猜。
````

## 8. 断连与背压边界回归

- `RouteMgr` 的 `hold pooled reconnect bad=0` 必须在主动断开后不 yield、不等待清理，立即向同一连接发 RPC，验证重连排队。
- `hold route backpressure status bad=0` 检查已连接发送及建连后的缓存补发均准确返回 BUSY，背压不换会话，随后的小请求仍可成功。`BudgetMgr` 同时检查满载返回 BUSY、关闭返回 CONNECT。
- `RpcPendingMgr` 的 `hold disconnect drain calls=4096 bad=0` 跨 64 个分片及固定大小批次清理 4096 项，首个完成回调注入 bad_alloc，验证其他回调继续、结果全部为 CONNECT、保护计数归零、等待索引恢复基线。清理本身不动态扩容；该注入发生在完成回调，不是全局内存耗尽模拟。
- 以上均由常规 `--case all` 的 Hold 触发，并由回归脚本断言。


## 9. Redis 类型化单 key 命令回归

常规 RedisMgr 完整性测试及压力测试使用标准命令接口，原生 CallRedis 保留给 PING、Lua 和协议/状态隔离探针。强制标记为 `redis typed commands string rpcdata zset bad=0`，覆盖二进制 key/value、空字符串/缺失值、WRONGTYPE、rpcdata 正常/损坏、集合中途解码失败不覆盖原输出、hash rpcdata、排序集合正反序/分数/更新计数、NaN 拒绝和共享 Task 重复等待。同步压力线程同时覆盖 string 与 rpcdata 存取。所有标准命令只使用一个 key，不以批量多 key 指令代替逐条调用。


Redis 类型回归还必须确认 `redis typed collections string rpcdata bad=0`、`redis typed native ping lua scalar array rpcdata bad=0` 和两个 `redis typed sync id=0/1 bad=0` 标记。覆盖 string/hash/list/set/zset 常用读写/计数/删除，空值与错误类型，Lua 返回整数/布尔/字符串/数字字符串/数组/rpcdata、Lua 错误与 nil、整数溢出和结果形状不匹配；所有失败保持原输出。同步线程覆盖 PING、Lua、字符串、rpcdata、列表、hash、set、zset 与分数。Lua 脚本若需访问存储，只显式声明一个 key；不新增隐式多 key 用例。

新增五类单 key 命令的 RedisMgr 回归要求 `redis expanded five types bad=0`，覆盖条件写、增减、批量与空参数、二进制字符串、rpcdata、带 nil 的 HMGET/ZMSCORE、正反范围及弹出删除。HSCAN、SSCAN、ZSCAN 累积到游标为零，不假定 COUNT 指定返回数量；同步压力线程也检查 optional 及三种扫描的 rpcdata 解码。

Redis 断言按每条命令分别等待、检查返回值和输出，失败日志包含检查行号。GCC13 下复合多 await 条件复用输出曾造成 6 个集合断言失败，布尔位数组代理曾在读取时崩溃；不要通过减少断言或跳过 Linux 消除此类失败。成员批量判断使用 0/1 整数数组，原生类型化调用也拒绝 vector<bool>，布尔标量仍支持。



## 10. 原始异常日志回归

`redis`、`rpc-ingress`、`hold` 三项增加 `diagnostic original errors persisted <case> PASS` 标记。测试通过正常退出排空日志服后检查真实 error 文件，不能只检查客户端打印：

- Redis 检查 Lua 原始错误和 WRONGTYPE 的终端记录，并检查 Lua 错误及请求/会话/命令信息已落盘；原有标准返回码和输出不变断言继续执行。
- RPC 入站测试发送合法帧包裹损坏的字段头，验证失败回执调用号仍为 424242，并确认解包阶段、会话、协议号、调用号和 ParserException 已落盘。
- Hold 触发 handler 异常和响应序列化异常，确认两种原始异常消息与调用上下文已落盘；失败不覆盖输出，后续请求正常恢复。另覆盖诊断文本的换行、NUL、反斜杠转义、百分号原样保留和 4096 字节截断边界。

上述检查复用现有模块与开关，纳入 `--case all`，Windows/Linux 单、多线程均需执行。它们验证正常日志管道可落盘，不代表日志服断连、磁盘损坏或进程被强杀时仍保证日志持久化。



## 11. 直连背压、客户端解包与 Redis 上游断连回归

普通 Redis 用例另启用临时 `Regression.redis_send_budget=true`，只将该客户端的 `max_send_queue_bytes` 设为 1MiB。循环 8 次在先发小请求之后分别通过原生回执、类型化输出接口拒绝 2MiB 请求，再发送小请求；检查 16 次拒绝均为 BUSY、失败不覆盖输出、16 次小响应内容正确、连接未重建、等待项清零。通过标记为 `redis send budget rejected=16 recovered=16 pending=0 bad=0`，并检查 `stage=send` 的容量错误原因已由日志服落盘。

`redis-shutdown` 临时配置的请求超时为 10 秒，避免 Windows 启动控制台通知辅助进程时先触发原来的 300ms 请求超时；仍要求停服唤醒为 Disconnected、停止后新调用立即失败、等待项清零，并保留通知后的 3 秒退出时限。短超时正确性由独立 `redis-queue-timeout` 用例验证，不把 Timeout 当作停服成功。

- `BackpressureMgr` 输出 `hold direct backpressure queued=... direct=... pinned=... closed=... bad=0`：常驻测试管理器使用 256 字节真实会话发送预算，超过预算的请求分别在建连前排队、已连接直发和显式 sid 路径返回 BUSY，输出保持原值；小请求正常完成，关闭后的显式 sid 调用返回 CONNECT。
- `Regression.encoding_failure` 的现有服务端注入同时支持合法外层回执中的损坏字段体。`EncodeMgr` 检查客户端返回 FAIL、输出不被半包覆盖、后续正常请求成功，并输出 `hold client malformed response sid=... bad=0`。驱动核对日志服 error 文件中的 `decode_response`、实际来源 sid、调用号、协议号和原始 ParserException。
- 普通 Redis 用例的临时配置启用 `Regression.redis_upstream_disconnect`。RedisMgr 启动阻塞请求并通知既有驱动，驱动仅通过自己持有的进程句柄终止本次 Redis，然后复用持久可执行文件重启；不按进程名或端口杀服务、不删除安装。等待请求失败且输出不变，代理自动重连后 PING 返回 PONG，输出 `redis upstream disconnect recovery bad=0`。驱动还核对日志服落盘记录的上游地址、非零错误状态和原始 hiredis 原因。

全部纳入现有 `--case all` 的 Hold / Redis，用已有模块及开关复测，不新增独立客户端。Windows/Linux 单、多线程各跑完整矩阵；故障注入只影响本次独立 Redis，不操作用户 6379 服务。

## 12. 阻塞工作管道回归

阻塞池线程访问回归用同步具名函数调用会话管理器发送、会话发送及发送许可入口：单线程构建要求发送、连接启动、监听、UDP 对端创建、重连、路由通知、会话关闭、注册发现、Redis 同步调用及定时器操作返回 `OI_ERR_WRONG_THREAD`，并检查 UDP 对端输出参数、发送队列、事件掩码和已登记定时器不变、被拒绝的定时器闭包已释放；多线程构建核对空路由、空负载发送及入队行为。正常线程检查发送和定时器返回 `OI_OK`，未登记定时器返回 `OI_ERR_NOT_FOUND`。Redis 单线程模式还从阻塞池工作线程检查同步调用被拒绝，保留 poll 线程同步收发验证。随后原有管道、定时器与 RPC 检查继续验证 poll 活性。驱动检查 `hold blocking access bad=0`。

`BlockingMgr` 通过 `uv_pipe` 创建阻塞管道，在后台普通同步函数中调用 `uv_fs_read`；后台回调只执行普通同步代码，不调用 `co_await`、`StartTask` 或其他框架协程接口。空管道读开始后，前台协程计时 200 毫秒并完成真实 RPC，确认写入前读尚未结束；随后向管道写入 `pipe-ok`，等待读取结束并检查内容与返回码。异常和写入失败路径也要释放读端并等待后台结束。另以固定种子 20261110 在前台生成 1–20 毫秒延迟，128 个后台任务分别同步等待并返回不同值，按提交顺序核对 WhenAll 结果，不要求实际完成顺序必须变化。驱动强制检查 `hold blocking delay seed=20261110 min_ms=1 max_ms=20`、`hold blocking read started=1 timer=1 rpc=1 early=0 content=1 bad=0`，以及 `hold blocking tasks=128 waiters=64 runs=1 bad=0`。
