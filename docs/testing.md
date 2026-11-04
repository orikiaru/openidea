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

测试占用本机端口 19995、17981、16379、16380、17990、17992、17993。测试 Redis 用 16380，不用 6379：Windows 动态端口段可能被 Hyper-V/WinNAT 保留，本机已有的 6379 Redis 也不会和测试冲突。复制出的 `redisserver.toml`、`registry.toml` 由脚本改到 16380，仓库里的配置仍是 6379。运行前脚本探测占用；不杀其他人的服务。两端共享本机端口，**不要并行跑平台或线程模式**。

Windows 使用 `--redis-wsl` 时只有测试 Redis 在 WSL，所有待测模块仍为 Windows `.exe`。需要 WSL 的 localhost 转发可用。Redis 为脚本独立启动的临时实例，内存上限 1536MiB、关闭持久化，绝不借用现有数据库。

## 3. 推荐的一键矩阵

Windows 宿主 PowerShell，同一次调用顺序运行 Windows、Linux：

```powershell
python tools/test_matrix.py --platform both --redis-wsl --wsl-user jxy --asan --clock-faults
```

只跑 Windows：

```powershell
python tools/test_matrix.py --redis-wsl --wsl-user jxy --build-dir build-win
```

只跑 Linux，在 WSL：

```sh
python3 tools/test_matrix.py --build-dir "$HOME/openidea-build-gcc13" --asan --clock-faults
```

自定义路径示例：

```powershell
python tools/test_matrix.py --platform both --redis-wsl --wsl-user jxy --linux-build-dir /home/jxy/openidea-build-gcc13 --redis-binary /tmp/redis-7.4.12/src/redis-server --artifacts C:/Users/Admin/openidea-test-results/manual-run
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
python tools/run_regressions.py --case all --bin-dir deploy/bin/Release --redis-wsl --wsl-user jxy --artifacts C:/Users/Admin/openidea-test-results/direct
```

Windows 的日志故障测试要求该产物以 `-DOI_ENABLE_TEST_HOOKS=ON` 构建。矩阵入口自动设置和恢复；直接运行入口检查构建标记，缺失则失败，不能跳过。

主要成功标记：

- echo / registry：`ok=1000000 mismatch=0 rpc_fail=0`，连接只启动一次。
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

## 7. 2026-11-04 验证记录

- Windows：MSVC 19.41.34123、SDK 10.0.26100，原生 x64 Release，MT/ST 各 18 项通过。
- Linux：WSL GCC 13.1，MT/ST 各 18 项通过；两模式前跳/后跳故障和纯协程 ASan 均通过。响应结果顺序修复后又跑完两模式全量及 ASan。
- 最终 Windows MT 证据在宿主 `~/openidea-test-results/final-20261104/windows-final-mt`；ST 在 `windows-fixed/st`。后者同轮的旧 MT 失败不能计为通过，已由修复后 MT 完整覆盖。
- 最终 Linux 证据在 WSL `~/openidea-test-results/final-20261104-response-fix`；校时证据在宿主 `~/openidea-test-results/final-20261104/linux`。
- 最终四组逐例结果汇总在宿主 `~/openidea-test-results/final-20261104/verified.json`，共 72 项。日志与大文件不入库。
- 本轮修复 Windows 分支变量重命名遗漏、系统 min/max 宏干扰、RPC 解码和响应移动的顺序，以及 Windows 到 WSL 的路径传递；相关失败日志保留在原结果目录。
- 早期 Linux MT 压测曾返回 291 次 OI_ERR_BUSY，独立复测及后续完整矩阵均通过。开放式百万请求压力在积压时可以触发容量保护，不能把重试通过等同于任何负载都不会限流。
