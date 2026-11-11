# 时间

秒用 `GetTime()`。要等一段时间用协程的 [`yield`](coro.md)。时间轮的细节见 [定时器](../timer.md)。

## 接口

| 用途 | 声明 |
| --- | --- |
| 墙上时间，秒 | [`OI::Timer::GetTime`](../../cbaselib/timer/inc/oi_timer_helper.h#L36) |
| 时间轮上的秒 | [`BaseTimerManager::GetTime`](../../cbaselib/timer/inc/oi_timer_manager.h#L71) |
| 单调毫秒 | [`SteadyMilliseconds`](../../cbaselib/timer/inc/oi_duration.h#L107) |
| 挂一个定时器 | [`AddTimer`](../../cbaselib/timer/inc/oi_timer_manager.h#L98) |
| 单例声明 | [`DECLARE_SINGLETON`](../../cbaselib/base/inc/oi_singleton.h#L143) |
| 单例跟着时间轮更新的说明 | [`oi_singleton.h`](../../cbaselib/base/inc/oi_singleton.h#L14) |

```cpp
::OI::Timer::BaseTimerManager::GetInstance().AddTimer([iSessionId]()
{
	RoomMgr::GetInstance().ExpireRead(iSessionId);
}, 2000);
```

想让单例每跳或每秒被调用，在类里写公有的 `OI::Coro::Task OnTickUpdate(uint64_t)` 或 `OI::Coro::Task OnSecondUpdate(time_t)`。两个只能写一个，都写会编译失败；返回类型不是 `OI::Coro::Task` 也会 `static_assert` 失败。不写就不注册。

> **注意：** 不自己用 `std::chrono` 做业务计时。秒够用就用 [`GetTime`](../../cbaselib/timer/inc/oi_timer_helper.h#L36)。

> **注意：** 时间轮在 poll 线程推进 tick 后，为每个注册的单例 `StartTask` 一条独立协程，只启动、不等待。单线程放进本线程的运行队列，多线程放进工人队列。里面可以 `co_await`（RPC、Redis、`yield` 等），不能调 `CallRedisSync`、`Lookup`、`PullSync`、`Publish`、`sleep`。同一单例上一条没 `co_return` 之前不会再启动，期间到点的跳或秒直接跳过，不补，也不排队。抛出的异常交给任务错误处理器报告，在跑标志清掉后，下一跳或下一秒照常启动。

> **注意：** `RunFramework` 开始之前不分发。首次 `GetInstance` 构造完成后注册，析构前注销；注销后不再启动新的，已经在跑的协程不取消。`main` 里的 `Init` 做完之后，下一跳才开始启动。错过的秒不补。

> **注意：** `AddTimer` 的 lambda 在 poll 线程上直接执行。回调要短。返回 true 的周期定时器会按 tick 续上。
