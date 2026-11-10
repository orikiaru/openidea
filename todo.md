# 待办

## hold 的 redis late ping 偶发超时

- 现状：`hold` 的 `CheckRedisLateReply` 先发 `BLPOP hold-late 3`，客户端 2 秒判超时，接着在同一条连接上发 PING。正常 PONG 排在 BLPOP 的 nil 后面，约 1 秒到（`late ping ok=1 wall_ms≈900~1100`）。linux-mt 上偶发 `late ping ok=0 status=1 reply_type=-1 wall_ms≈2010`，PONG 在 PING 自己的 2 秒超时内没回来，`hold done bad=1`。2026-11-11 单独重跑三次挂一次，无并行负载时也会挂；Windows 和 linux-st 没见过。
- 方向：先抓一次失败时 redisserver 和 Redis 两边的时间线，确认 PONG 是晚到、被丢，还是被记到超时那条命令上。再决定是修 Redis 客户端/redisserver 的迟到回复处理，还是放宽用例的时间余量。
