# 服务名

配置里写了 `<名字>.service` 时，`SessionManager::Init` 先向 registry 查地址，查不到再用同一段的 ip/port。表怎么存、分区怎么选，见 [服务注册](../registry.md)。

## 接口

| 用途 | 声明 |
| --- | --- |
| 客户端 | [`RegistryClient`](../../cbaselib/base/inc/oi_registry.h#L101) |
| 查一个地址 | [`Lookup`](../../cbaselib/base/inc/oi_registry.h#L109) |
| 同步拉全部分区 | [`PullSync`](../../cbaselib/base/inc/oi_registry.h#L112) |
| 异步拉 | [`PullAsync`](../../cbaselib/base/inc/oi_registry.h#L115) |
| 监听成功后登记 | [`Publish`](../../cbaselib/base/inc/oi_registry.h#L122) |
| 不限分区调用 | [`CallAny`](../../cbaselib/proto/inc/oi_rpc_session_manager.h#L208) |

> **注意：** 应用只连 registry，不自己扫 Redis 里的服务表。

> **注意：** [`Lookup`](../../cbaselib/base/inc/oi_registry.h#L109)、[`PullSync`](../../cbaselib/base/inc/oi_registry.h#L112)、[`Publish`](../../cbaselib/base/inc/oi_registry.h#L122) 要真正等待时，在 poll 回调里调用会抛 `logic_error`。协程里用 [`PullAsync`](../../cbaselib/base/inc/oi_registry.h#L115)。

> **注意：** `PullAsync` 的回调只调用一次，可能在 poll 线程上。回调里不能再等注册表。
