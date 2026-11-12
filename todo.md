# 待办

## hold 的 redis late ping 偶发超时

- 现状：`hold` 的 `CheckRedisLateReply` 先发 `BLPOP hold-late 3`，客户端 2 秒判超时，接着在同一条连接上发 PING。正常 PONG 排在 BLPOP 的 nil 后面，约 1 秒到（`late ping ok=1 wall_ms≈900~1100`）。linux-mt 上偶发 `late ping ok=0 status=1 reply_type=-1 wall_ms≈2010`，PONG 在 PING 自己的 2 秒超时内没回来，`hold done bad=1`。2026-11-11 单独重跑三次挂一次，无并行负载时也会挂；Windows 和 linux-st 没见过。
- 诊断：2026-11-11 在隔离的 Linux MT 路复跑四次均通过，迟到 PING 分别耗时 1040、910、844、849 毫秒。后三次通过 Redis MONITOR 观察到 BLPOP 和 PING 在同一上游连接，PING 在 BLPOP 后约 3.01～3.08 秒执行；这不能证明回复已送达客户端。未找到原失败日志，尚未抓到失败时的时间线，根因未确认。
- 方向：先抓一次失败时 redisserver 和 Redis 两边的时间线，确认 PONG 是晚到、被丢，还是被记到超时那条命令上。再决定是修 Redis 客户端/redisserver 的迟到回复处理，还是放宽用例的时间余量。

## Windows ST disk-full 清理通知 helper 偶发退出 120（2026-11-12）

- 现状：本轮两次 Windows ST 全量均为 20/21；disk-full 已成功检测 ENOSPC，但清理通知 helper 退出码为 120。增加 stdout/stderr 捕获和失败诊断后定点运行通过，模拟退出 120 仍会报错；根因尚未修复或确认。
- 证据：本地结果 `windows-st-latest` 和 `windows-st-disk-diagnostic`。
- 方向：保留现有证据，下次失败时捕获 stderr，核实控制台与标准流生命周期。禁止吞掉退出码 120 或放宽断言。

## C++ 实际命名 AST 扫描缺依赖（2026-11-12）

- 现状：缺少 Python clang/libclang，实际命名 AST 扫描受阻。命名规则自身 9 项测试通过，继承检查和格式检查通过，但不能代替实际扫描。
- 方向：持久安装所需依赖，使用现有 `tools/check_field_names.py` 根据编译数据库执行扫描，并区分存量问题。
