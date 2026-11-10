# 锁

自有代码的锁在 `oi_lock.h`。单线程编译下这些加锁和等待是空操作。

## 接口

| 用途 | 声明 |
| --- | --- |
| 这一组锁的说明 | [`oi_lock.h`](../../cbaselib/base/inc/oi_lock.h#L15) |
| 互斥 | [`Mutex`](../../cbaselib/base/inc/oi_lock.h#L120) |
| 条件等待 | [`MutexCond`](../../cbaselib/base/inc/oi_lock.h#L153) |
| 互斥的守卫 | [`MutexGuard`](../../cbaselib/base/inc/oi_lock.h#L203) |

```cpp
::OI::Thread::MutexGuard stLock(m_stMutex);
while (!bReady)
{
	m_stCond.Wait(m_stMutex);
}
```

> **注意：** 不直接写 `std::mutex`、`std::lock_guard`、`std::condition_variable`。要新的等待方式，先补 `oi_lock.h` 的单线程和多线程两套。

> **注意：** `MutexCond` 不带条件参数。等待写成 `while (条件不满足) { Wait(...); }`。带超时的用 `WaitUntil` 的返回值离开循环。

> **注意：** 单线程下锁不会真的阻塞。要等网络或定时器，改走 poll，不要靠锁空等。
