# 服务名单

业务进程只使用连接名单，以及要不要上报自己的地址。静态配置还是动态配置只写在 registry 进程上。现有行为见 `docs/registry.md`。

没有 `Registry.ip` 时，名单是 `[Registry] instances`。没有这份名单，就用各段自己的 ip/port。

配了 `Registry.ip` 时，业务进程向 registry 拉名单。回包字段 `bStatic` 为真只拉名单，为假登记并心跳；旧 registry 不带此字段时默认登记。

`Registry.config` 不写或是 `dynamic` 时是动态配置，回包 `bStatic` 为假。`static` 时是静态配置，读表 `oi_service_instance`，回包 `bStatic` 为真，不接受登记。现在动态用 Redis，静态用 MySQL，换引擎不改配置名。
