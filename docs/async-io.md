# 异步 IO

排期项，还没改代码。套接字从就绪通知改成提交后等完成。Linux 用 io_uring，Windows 用 IOCP。应用层的 `co_await`、会话和协议不动。

## 现在

`OI::Net::Poll` 包着 libuv。套接字走 `uv_poll`：描述符可读或可写之后，poll 线程里再 `read` / `write`。Linux 底下是 epoll，Windows 底下 libuv 自己用 IOCP 驱动循环，但套接字仍是就绪模型，不是完成端口上的 `WSARecv` / `WSASend`。

`CallOnLoop`、`PostOnLoop`、`PollWait`、`ArmTick` 都挂在这条循环上。单线程里 poll 在主线程，多线程里 poll 是单独一条线程。时间轮由这条循环按 `offset_ms` 叫醒，到点再把 `Update` 放进任务队列，不在回调里转轮子。

## 目标

poll 线程提交接收、发送、接入和连接，完成队列里拿到结果再进现有的 `DataIn` / `DataOut`。Linux 一个 io_uring，Windows 一个 IOCP。两边对上层是同一套完成，不把 epoll 和 IOCP 的差别漏到 `Session`。

先换 `Poll` 和 `Channel` 的等待与提交。RPC、Redis、HTTP、日志仍从会话收发，不各写一套环或完成端口。

## 保持不变

- 仍是一条 poll 线程。io_uring 和 IOCP 都不交给工人线程抢。
- `PollWait(0)` 只取已经完成的，不堵。没有完成也没有待跑任务时才堵住。
- 完成回调里不 `resume` 协程，只 `Schedule`。poll 回调里仍然不能 `CallRedisSync` 这类同步等待。
- 时间轮仍是叫醒后把 `Update` 放进队列。io_uring 用超时完成来叫醒，Windows 用可等待定时器或 `GetQueuedCompletionStatusEx` 的超时，不在完成回调里转轮子。
- 单线程、多线程都要跑现有回声、Hold、Redis、Web。行为变了才算这一项做完。

## 不做在第一刀里

注册缓冲、多射接收、批量提交留到完成路径稳定之后。域名解析现在走 libuv 线程池，先留着，不和套接字完成绑在一起。`webserver` 的 cpp-httplib 监听不纳入这一刀。
