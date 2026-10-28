# 接入一条连接

可读发生在主线程的 `PollWait` 里。新连接的读事件也在这次回调里当场挂上。`OnAddSession` 若拉起协程，句柄只进队列，要等这次 `uv_run` 返回之后的下一轮才会 `resume`。

```mermaid
sequenceDiagram
    autonumber
    participant Main as 主线程
    participant Uv as libuv
    participant Acc as AcceptChannel
    participant Help as NetHelper
    participant Mgr as SessionManager
    participant Sess as ProtoSession
    participant Tcp as TCP_Channel
    participant Queue as StScheduler

    Main->>Uv: PollWait 里的 uv_run
    Uv->>Acc: DataIn
    loop 直到 SocketAcceptOne 返回 0
        Acc->>Help: SocketAcceptOne
        alt accept 拿到 fd
            Help->>Mgr: AddSession 新 fd
            Mgr->>Sess: CloneSession
            Mgr->>Tcp: new TCP_Channel
            Mgr->>Main: 只注册读，回调里当场 uv_poll_start
            Mgr->>Mgr: 分配 sid 放进表
            Mgr->>Mgr: OnAddSession
            opt OnAddSession 里 StartTask
                Mgr->>Queue: 句柄入队
                Note over Queue: 这一轮 uv_run 还没返回，不会 resume
            end
            Help-->>Acc: 1 接着取
        else EAGAIN
            Help-->>Acc: 0 退出循环
        else 其他错误
            Help-->>Acc: -1 打日志后 assert
        end
    end
```

`EINTR` 在 `SocketAcceptOne` 里面重试，不回到这层循环。

对端关掉或读出错时，先把已经读到的字节交给 `OnDataIn`，再 `PassiveClose`。
