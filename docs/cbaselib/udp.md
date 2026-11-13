# UDP

组件配置 `type = "udp"`。一个绑好的口，按对端地址一条会话。业务仍走 `Session` 和 RPC，不自己 `recvfrom` / `sendto`。标准例子是 [`IpcUdp`](../../modules/ipcserver/inc/ipcudp.h#L11)。

## 接口

| 用途 | 声明 |
| --- | --- |
| 绑端口并开始收 | [`StartServer`](../../cbaselib/net/inc/oi_session_manager.h#L171) |
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

> **注意：** 没有 `StartClient`。主动发之前要先 `StartServer`，再用 `OpenPeer`。端口写 `"0"` 时用 [`LocalPort`](../../cbaselib/net/inc/oi_session_manager.h#L149) 取系统分配的端口。

> **注意：** `OpenPeer` 只认 IPv4/IPv6 字面地址，不解析域名。地址不对返回 `OI_ERR_INVALID_ARGUMENT`，还没 `StartServer` 返回 `OI_ERR_CONNECT`；失败时不修改输出会话号。

> **注意：** 一包必须装下整帧。超过 IPv4 的 65507 字节或 IPv6 的 65527 字节返回 `OI_ERR_INVALID_ARGUMENT`。`sendto` 没接住整包返回 `OI_ERR_FAIL`。这两种都丢掉这一包，不调用 `OnBytesSend`。

> **注意：** UDP 绑定不开 `SO_REUSEADDR`，两个进程抢不到同一个口。UDP 口不向 registry 登记。`idle_timeout_ms` 默认 60000，这段时间没收没发就关会话。
