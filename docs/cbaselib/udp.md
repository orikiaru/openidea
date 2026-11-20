# UDP

组件配置 `type = "udp"`。绑定通道是唯一调用 `recvfrom` 的通道，绑定后还没有会话。每个对端地址出现时才建一条会话及其发送通道；每条发送通道持有由绑定套接字复制的独立描述符，因此源端口一致。业务发送时通过会话号（sid）选择对端。业务仍走 `Session` 和 RPC，不自己 `recvfrom` / `sendto`。标准例子是 [`IpcUdp`](../../modules/ipcserver/inc/ipcudp.h#L11)。

## 接口

| 用途 | 声明 |
| --- | --- |
| 绑定本端口并开始收 | [`StartServer` / `StartClient`](../../cbaselib/net/inc/oi_session_manager.h) |
| 按字面地址换成 sid | [`OpenPeer`](../../cbaselib/net/inc/oi_session_manager.h#L151) |
| 这条会话的对端 | [`GetPeerAddr`](../../cbaselib/net/inc/oi_session_manager.h#L153) |
| 实际绑上的本端端口 | [`LocalPort`](../../cbaselib/net/inc/oi_session_manager.h#L149) |
| 发出一包 | [`SendBytes`](../../cbaselib/net/inc/oi_session_manager.h#L184) |

```toml
[IpcUdp]
ip = "127.0.0.1"
port = "0"
type = "udp"
```

```cpp
sid_t iSessionId = INVALID_SID;
const int32_t iCode = OpenPeer(sIp, sPort, iSessionId);
if (iCode != OI_OK)
{
	return iCode;
}
```

> **注意：** `StartServer` 和 `StartClient` 都只绑定配置的本端地址和端口，不指定远端；`co_await StartClient()` 可等待绑定结果。主动发送之前，先启动，再用 `OpenPeer` 显式指定对端并取得 sid。端口写 `"0"` 时用 [`LocalPort`](../../cbaselib/net/inc/oi_session_manager.h#L149) 取系统分配的端口。

> **注意：** `OpenPeer` 只认 IPv4/IPv6 字面地址，不解析域名。地址不对返回 `OI_ERR_INVALID_ARGUMENT`，还没启动返回 `OI_ERR_CONNECT`；失败时不修改输出会话号。

> **注意：** 一个数据报必须装下完整帧。发送成功表示按 sid 入队；相邻 RPC/Notify 完整帧可自动合并，目标大小 1200 字节，超过目标的单帧仍单独发送。原始数据按次独立发送，即使外观像帧也不合并。端口以一个 peer 到通道的哈希表管理对端，遍历有包的通道，每个 peer 使用独立队列；成功不表示对端已收到。遇到 `EAGAIN` 保留队首，等可写后重试。超过 IPv4 的 65507 字节或 IPv6 的 65527 字节、队列繁忙或 sid 已失效时立即拒绝。入队后 `sendto` 发生硬错误或未接住整包时丢弃该包，不自动重发，也不调用 `OnBytesSend`。

> **注意：** 服务端收到请求时可使用请求上下文中的 sid 向该对端发起后续调用或通知；主动向尚未发包的对端发送时，须先用 `OpenPeer` 取得 sid。不要用绑定口代替对端会话号。

> **注意：** UDP 绑定不开 `SO_REUSEADDR`，两个进程抢不到同一个口。UDP 口不向 registry 登记。管理器按 `idle_timeout_ms` 检查会话活跃时间（默认 60000），这段时间没收没发就关闭会话；后续来包或 `OpenPeer` 会创建新会话。
