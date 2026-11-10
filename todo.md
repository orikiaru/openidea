# 待办

## hold 的 redis late ping 偶发超时

- 现状：`hold` 的 `CheckRedisLateReply` 先发 `BLPOP hold-late 3`，客户端 2 秒判超时，接着在同一条连接上发 PING。正常 PONG 排在 BLPOP 的 nil 后面，约 1 秒到（`late ping ok=1 wall_ms≈900~1100`）。linux-mt 上偶发 `late ping ok=0 status=1 reply_type=-1 wall_ms≈2010`，PONG 在 PING 自己的 2 秒超时内没回来，`hold done bad=1`。2026-11-11 单独重跑三次挂一次，无并行负载时也会挂；Windows 和 linux-st 没见过。
- 方向：先抓一次失败时 redisserver 和 Redis 两边的时间线，确认 PONG 是晚到、被丢，还是被记到超时那条命令上。再决定是修 Redis 客户端/redisserver 的迟到回复处理，还是放宽用例的时间余量。

## 通知走连接的暂存和重连

- 现状：`NotifyPeer` / `NotifyAny` 先取会话号再 `RpcNotify`。调用号在线上是 0，对端不回包。会话号是锁外的快照，断线重连换了新会话，这一包就打到旧会话上。`Call` / `CallAny` 走 `PeerConnection::Submit`：没连上先暂存并 `Ensure`，连上后在连接锁里用当前会话号发出。通知没进这条路径。`ConnectedAny` 只解决“挑一台已连上的”，不解决重连。
- 方向：不要给通知再做一套调用号槽。直接套现有 RPC。`NotifyPeer` / `NotifyAny` 拿到 `RpcSender` 后起一个不等待的任务，里面 `co_await RpcCall(sender, msg)`，结果丢掉。没连上时就是现有的暂存和重连。对端按 RPC 回一个空响应：调用号 + 返回码，成功时再序列化一份默认的 `Response`。调用方不看回执。回执丢了就等这次调用超时，任务结束。线上调用号不能再写 0，否则对端不回包，这次包装会一直占到超时。调用号 0 仍表示旧的单向消息，不回包。
