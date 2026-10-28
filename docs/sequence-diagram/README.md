# 时序图

一帧是本机字节序的 `uint32` 协议号、`uint32` 包体长度、包体。RPC 的包体再以 `uint64` 调用号开头，后面才是 Req 或 Res。请求和回包各有自己的协议号，客户端按调用号认回包。

单线程的 poll 就在主线程上。多线程的 poll 是单独一条线程，协程在工人上 `resume`。两边各一套图。

## 单线程

- [进程起来](single-thread/startup.md)
- [服务端开始听](single-thread/listen.md)
- [接入一条连接](single-thread/accept.md)
- [客户端连上](single-thread/connect.md)
- [发出一次调用](single-thread/send.md)
- [收到一帧](single-thread/receive.md)
- [处理并回包](single-thread/reply.md)
- [调用超时](single-thread/timeout.md)
- [主循环](single-thread/loop.md)
- [让出](single-thread/yield.md)

## 多线程

- [进程起来](multi-thread/startup.md)
- [服务端开始听](multi-thread/listen.md)
- [接入一条连接](multi-thread/accept.md)
- [客户端连上](multi-thread/connect.md)
- [发出一次调用](multi-thread/send.md)
- [收到一帧](multi-thread/receive.md)
- [处理并回包](multi-thread/reply.md)
- [调用超时](multi-thread/timeout.md)
- [主循环](multi-thread/loop.md)
- [让出](multi-thread/yield.md)
