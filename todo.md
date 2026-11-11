# 待办

## hold 的 redis late ping 偶发超时

- 现状：`hold` 的 `CheckRedisLateReply` 先发 `BLPOP hold-late 3`，客户端 2 秒判超时，接着在同一条连接上发 PING。正常 PONG 排在 BLPOP 的 nil 后面，约 1 秒到（`late ping ok=1 wall_ms≈900~1100`）。linux-mt 上偶发 `late ping ok=0 status=1 reply_type=-1 wall_ms≈2010`，PONG 在 PING 自己的 2 秒超时内没回来，`hold done bad=1`。2026-11-11 单独重跑三次挂一次，无并行负载时也会挂；Windows 和 linux-st 没见过。
- 诊断：2026-11-11 在隔离的 Linux MT 路复跑四次均通过，迟到 PING 分别耗时 1040、910、844、849 毫秒。后三次通过 Redis MONITOR 观察到 BLPOP 和 PING 在同一上游连接，PING 在 BLPOP 后约 3.01～3.08 秒执行；这不能证明回复已送达客户端。未找到原失败日志，尚未抓到失败时的时间线，根因未确认。
- 方向：先抓一次失败时 redisserver 和 Redis 两边的时间线，确认 PONG 是晚到、被丢，还是被记到超时那条命令上。再决定是修 Redis 客户端/redisserver 的迟到回复处理，还是放宽用例的时间余量。
