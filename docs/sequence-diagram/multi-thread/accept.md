# 接入一条连接

可读或可写发生在 poll 线程。可写的 `DataOut` 直接转 `DataIn`。新连接只注册读，也在这次回调里当场 `uv_poll_start`。`OnAddSession` 里的 `StartTask` 放进工人队列。有空闲工人时，可以在这次回调返回前就 resume。

`EAGAIN` 和 `EINPROGRESS` 让 `SocketAcceptOne` 返回 0，外层循环结束。`EINTR`、`ECONNABORTED`、`EPROTO` 在 `SocketAcceptOne` 内部继续，不回到外层。返回 -1 时 `PauseEvents(this, 100)`，停 100 毫秒再恢复，避免描述符用尽时空转。意料外的 errno 会先打日志；还没 listen 这类检查失败也是 -1，不打这行日志。

注册新连接失败时关掉这个 fd 并释放会话，`SocketAcceptOne` 仍返回 1，外层继续收。

```mermaid
sequenceDiagram
    autonumber
    participant Poll as poll 线程
    participant Uv as libuv
    participant Acc as AcceptChannel
    participant Help as NetHelper
    participant Mgr as SessionManager
    participant Sess as ProtoSession
    participant Tcp as TCP_Channel
    participant Worker as 工人

    Poll->>Uv: uv_run
    Uv->>Acc: DataIn
    loop 直到返回 0 或 -1
        Acc->>Help: SocketAcceptOne
        alt accept 拿到 fd
            Help->>Mgr: AddSession 新 fd
            Mgr->>Sess: CloneSession
            Mgr->>Tcp: new TCP_Channel
            Mgr->>Poll: 只注册读，当场 uv_poll_start
            Mgr->>Mgr: 分配 sid 放进表
            Mgr->>Mgr: OnAddSession
            opt OnAddSession 里 StartTask
                Mgr->>Worker: PostCoroutine
                Note over Worker: 有空闲工人时，回调返回前就可能 resume
            end
            Help-->>Acc: 1 接着取
        else EAGAIN 或 EINPROGRESS
            Help-->>Acc: 0 结束循环
        else 返回 -1
            Help-->>Acc: -1
            Acc->>Poll: PauseEvents(this, 100) 然后跳出
        end
    end
```
