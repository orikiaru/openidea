# 协程

任务、让出和后台阻塞函数都在 `OI::Coro`。语义见 [协程](../coroutine.md)。

## 接口

| 用途 | 声明 |
| --- | --- |
| 拉起一条独立任务 | [`StartTask`](../../cbaselib/base/inc/oi_coro.h#L666) |
| 空等若干毫秒 | [`yield`](../../cbaselib/base/inc/oi_coro.h#L113) |
| 等一组任务 | [`WhenAll`](../../cbaselib/base/inc/oi_coro.h#L608) |
| 把同步函数丢到后台 | [`RunBlocking`](../../cbaselib/base/inc/oi_coro.h#L505) |
| 单线程启动检查 | [`CheckTaskStartThread`](../../cbaselib/base/inc/oi_coro.h#L54) |

```cpp
::OI::Coro::StartTask([](::OI::Coro::Token)
{
	co_await ::OI::Coro::yield(1000);
});

const int32_t iRet = co_await ::OI::Coro::RunBlocking(ReadFile, sPath, sData);
```

> **注意：** 应用层空等只用 `co_await OI::Coro::yield`。不要 `sleep`、`sleep_for`、`Sleep`、`usleep`。单线程只有一条线程，睡下去 poll 停转。

> **注意：** 单线程下，创建和 `StartTask` 必须在 poll 线程。别的线程会先打 warning，再抛 `std::logic_error`，任务不会入队。多线程不限制启动线程。

> **注意：** `RunBlocking` 的函数必须返回 `int32_t`。左值参数是借用，输出对象要活到这次工作结束。丢弃返回的 Task 不会取消工作。后台函数不能去碰单线程的业务管理器。在途最多 1024 个，满了返回 `OI_ERR_BUSY`。

> **注意：** 依赖上一步输出的多个 `co_await` 拆成独立语句。不要写进同一个 `&&` / `||`。

> **注意：** `WhenAll` 汇合 `TaskResult<bool>` 时得到 `vector<int32_t>`。不要用 `vector<bool>`。
