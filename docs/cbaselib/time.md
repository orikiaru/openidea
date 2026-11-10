# 时间

秒用 `GetTime()`。要等一段时间用协程的 [`yield`](coro.md)。时间轮的细节见 [定时器](../timer.md)。

## 接口

| 用途 | 声明 |
| --- | --- |
| 墙上时间，秒 | [`OI::Timer::GetTime`](../../cbaselib/timer/inc/oi_timer_helper.h#L36) |
| 时间轮上的秒 | [`BaseTimerManager::GetTime`](../../cbaselib/timer/inc/oi_timer_manager.h#L71) |
| 单调毫秒 | [`SteadyMilliseconds`](../../cbaselib/timer/inc/oi_duration.h#L107) |
| 挂一个定时器 | [`AddTimer`](../../cbaselib/timer/inc/oi_timer_manager.h#L98) |
| 单例声明 | [`DECLARE_SINGLETON`](../../cbaselib/base/inc/oi_singleton.h#L130) |
| 单例跟着时间轮更新的说明 | [`oi_singleton.h`](../../cbaselib/base/inc/oi_singleton.h#L12) |

```cpp
::OI::Timer::BaseTimerManager::GetInstance().AddTimer([iSessionId]()
{
	RoomMgr::GetInstance().ExpireRead(iSessionId);
}, 2000);
```

想让单例每跳或每秒被调用，在类里写公有的 `OnTickUpdate(uint64_t)` 或 `OnSecondUpdate(time_t)`。不写就不注册。

> **注意：** 不自己用 `std::chrono` 做业务计时。秒够用就用 [`GetTime`](../../cbaselib/timer/inc/oi_timer_helper.h#L36)。

> **注意：** `OnTickUpdate` 和 `OnSecondUpdate` 在 poll 线程上同步调用。里面不要 `co_await`，也不要等待。要等结果就 `StartTask`。

> **注意：** `RunFramework` 开始之前不分发这些回调。`main` 里的 `Init` 做完之后，下一跳才开始调用。错过的秒不补。

> **注意：** `AddTimer` 的 lambda 在 poll 线程上直接执行。回调要短。返回 true 的周期定时器会按 tick 续上。
