# 服务名

配置里写了 `<名字>.service` 时，`SessionManager::Init` 先 `Lookup` 同步拉一次，查不到再用同一段的 ip/port。表怎么存、分区怎么选，见 [服务注册](../registry.md)。

## 接口

| 用途 | 声明 |
| --- | --- |
| 客户端 | [`RegistryClient`](../../cbaselib/proto/inc/oi_registry_client.h#L18) |
| 起服登记 | [`Publish`](../../cbaselib/proto/inc/oi_registry_client.h#L26) |
| 起服同步拉 | [`PullSync`](../../cbaselib/proto/inc/oi_registry_client.h#L29) |
| 起服按最小实例号选 | [`Lookup`](../../cbaselib/proto/inc/oi_registry_client.h#L31) |
| 运行期读缓存 | [`Snapshot`](../../cbaselib/proto/inc/oi_registry_client.h#L33) |
| 秒更新 | [`OnSecondUpdate`](../../cbaselib/proto/inc/oi_registry_client.h#L43) |
| 按服务名任选一台 | [`RouteManager::Call`](../../cbaselib/proto/inc/oi_rpc_session_manager.h#L437) |

> **注意：** 应用只连 registry，不自己扫 Redis 里的服务表。

> **注意：** [`RegistryClient`](../../cbaselib/proto/inc/oi_registry_client.h#L18) 继承 `ProtoSessionManager`。[`oi_registry.h`](../../cbaselib/base/inc/oi_registry.h) 只保留 [`ServiceInstance`](../../cbaselib/base/inc/oi_registry.h#L19)、[`ServiceTable`](../../cbaselib/base/inc/oi_registry.h#L37)、[`StaticRegistry`](../../cbaselib/base/inc/oi_registry.h#L95)。

> **注意：** [`Publish`](../../cbaselib/proto/inc/oi_registry_client.h#L26)、[`PullSync`](../../cbaselib/proto/inc/oi_registry_client.h#L29)、[`Lookup`](../../cbaselib/proto/inc/oi_registry_client.h#L31) 要真正等待时，在 poll 回调里调用会抛 `logic_error`。运行期选路读 [`Snapshot`](../../cbaselib/proto/inc/oi_registry_client.h#L33)，不在协程里同步拉。

> **注意：** 起服 `PullSync` / `Lookup` 同步强制拉并写入缓存。运行期连接、补登记、心跳、整量拉取由 [`OnSecondUpdate`](../../cbaselib/proto/inc/oi_registry_client.h#L43) 协程处理：每秒一次，上一次没结束就跳过；没连就连，到点登记或心跳，到 `pull_ms` 或 `Snapshot` 未命中催促时整量拉一次。`Snapshot` 未命中不立刻拉取，由下一次秒更新拉。路由发协议前读本地缓存；还没拉到过就等，等待上限是 min(剩余预算, 3000ms)。
