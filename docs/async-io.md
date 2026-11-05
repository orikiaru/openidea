# 异步 IO

排期项，还没改代码。主要是为了文件读写。普通文件在 epoll 上一直显示可读，真正的 `read` / `write` 会堵住调用线程，所以现在文件进不了 `Poll`。Linux 用 io_uring，Windows 用 IOCP，文件和套接字走同一条完成队列。应用层的 `co_await`、会话和协议不动。

## 现在

`OI::Net::Poll` 包着 libuv。套接字走 `uv_poll`：描述符可读或可写之后，poll 线程里再 `read` / `write`。Linux 底下是 epoll，Windows 底下 libuv 自己用 IOCP 驱动循环，但套接字仍是就绪模型，不是完成端口上的 `WSARecv` / `WSASend`。

文件不走这条路径。`logd` 的落盘在自己的写线程里用 `fwrite` 堵住写完，避免拖住 poll。别的文件读写也不能交给 poll 线程，一堵整条循环就停。

`CallOnLoop`、`PostOnLoop`、`PollWait`、`ArmTick` 都挂在这条循环上。单线程里 poll 在主线程，多线程里 poll 是单独一条线程。时间轮由这条循环按 `offset_ms` 叫醒，到点在回调里直接执行 `Update`；定时器回调只做短动作，协程只 `Schedule`。

## 目标

同一条完成队列同时收文件和套接字。Linux 一个 io_uring，提交文件的读、写和套接字的接收、发送、接入、连接。Windows 一个 IOCP，文件句柄用重叠 `ReadFile` / `WriteFile`，套接字用 `WSARecv` / `WSASend`。完成之后再进现有的文件回调或 `DataIn` / `DataOut`。两边对上层是同一套完成，不把环和完成端口的差别漏到业务。

文件是这一项的原因：读盘、写盘可以提交后继续跑 poll，不必再为每个文件养一条堵住的线程。套接字换完成模型是为了和文件共用这一条队列，不是另做一套网络库。

RPC、Redis、HTTP、日志仍从会话收发。`logd` 的字节和轮转规则不变，变的是 `fwrite` 改成提交写、完成后继续下一批。

## 保持不变

- 仍是一条 poll 线程。io_uring 和 IOCP 都不交给工人线程抢。
- `PollWait(0)` 只取已经完成的，不堵。没有完成也没有待跑任务时才堵住。
- 完成回调里不 `resume` 协程，只 `Schedule`。poll 回调里仍然不能 `CallRedisSync` 这类同步等待，也不能在里面同步读文件。
- 时间轮仍是叫醒后在 poll 线程直接 `Update`。io_uring 用超时完成来叫醒，Windows 用可等待定时器或 `GetQueuedCompletionStatusEx` 的超时。
- 单线程、多线程都要跑现有回声、Hold、Redis、Web，再加上日志落盘。文件写失败和磁盘满仍要能从现有错误路径看出来。行为变了才算这一项做完。

## 不做在第一刀里

注册缓冲、多射接收、批量提交留到完成路径稳定之后。域名解析现在走 libuv 线程池，先留着，不和套接字完成绑在一起。`webserver` 的 cpp-httplib 监听不纳入这一刀。
