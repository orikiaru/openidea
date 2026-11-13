# 进程启动

`main` 按这个顺序。标准例子是 [`ipcserver` 的 `main`](../../modules/ipcserver/src/main.cpp#L7)。

## 接口

| 用途 | 声明 |
| --- | --- |
| 读配置并初始化线程池、定时器、poll | [`StartFramework`](../../cbaselib/base/inc/oi_app.h#L9) |
| 进入主循环，直到停止 | [`RunFramework`](../../cbaselib/base/inc/oi_app.h#L11) |
| 日志客户端 | [`OI::Log::Init`](../../cbaselib/logger/inc/oi_log.h#L42) |
| 日志服自己写本地 | [`InitLocal`](../../cbaselib/logger/inc/oi_log.h#L45) |
| 打一条日志 | [`LOG_INFO`](../../cbaselib/logger/inc/oi_log.h#L54) |
| 服务端开始听 | [`StartServer`](../../cbaselib/net/inc/oi_session_manager.h#L171) |
| 客户端开始连 | [`StartClient`](../../cbaselib/net/inc/oi_session_manager.h#L167) |

```cpp
int iCode = ::OI::Base::StartFramework(pConfig);
if (iCode != 0)
{
	return iCode;
}
if (::OI::Log::Init() != OI_OK)
{
	return 1;
}
if (RoomServer::GetInstance().Init() != OI_OK || RoomServer::GetInstance().StartServer() != OI_OK)
{
	return 1;
}
return ::OI::Base::RunFramework();
```

> **注意：** `StartFramework` 失败已经往 stderr 打了原因。配置读不了返回 -2，线程池、定时器或 poll 失败返回 -1。`main` 把这个返回值原样交出去。

> **注意：** `logd` 不调用 `OI::Log::Init`。它在自己的管理器初始化之后调用 [`InitLocal`](../../cbaselib/logger/inc/oi_log.h#L45)。

> **注意：** `[ThreadPool] threadcount` 必须大于 0。多线程工人数从这里读。
