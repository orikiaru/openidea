# 接入一条连接

可读发生在 poll 线程。新连接的读事件在这次回调里当场挂上。`OnAddSession` 里的 `StartTask` 把句柄放进 `normal` 工人队列，工人可以在这次回调返回前就开始 `resume`。

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
    participant Worker as normal 工人

    Poll->>Uv: uv_run
    Uv->>Acc: DataIn
    loop 直到 SocketAcceptOne 返回 0
        Acc->>Help: SocketAcceptOne
        alt accept 拿到 fd
            Help->>Mgr: AddSession 新 fd
            Mgr->>Sess: CloneSession
            Mgr->>Tcp: new TCP_Channel
            Mgr->>Poll: 只注册读，回调里当场 uv_poll_start
            Mgr->>Mgr: 分配 sid 放进表
            Mgr->>Mgr: OnAddSession
            opt OnAddSession 里 StartTask
                Mgr->>Worker: PostCoroutine
                Worker->>Worker: resume
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
