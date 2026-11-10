# Redis

客户端连的是 `redisserver`，不是 Redis 本身。常用命令在 `oi_redis_cmd.h`，字符串这一类的声明在 `oi_redis_string.h`。命令表见 [基础库](../cbaselib.md) 的 redis 段。标准例子是 [`redis_mgr.cpp` 的 `Check`](../../modules/ipcclient/src/redis_mgr.cpp#L565)。

## 接口

| 用途 | 声明 |
| --- | --- |
| 读一个 key | [`RedisGet`](../../cbaselib/redis/inc/oi_redis_string.h#L11) |
| 写一个 key | [`RedisSet`](../../cbaselib/redis/inc/oi_redis_string.h#L93) |
| 带毫秒 TTL 的写 | [`RedisSet`](../../cbaselib/redis/inc/oi_redis_string.h#L211) |
| 原生命令，协程 | [`CallRedis`](../../cbaselib/redis/inc/oi_redis.h#L153) |
| 原生命令，同步 | [`CallRedisSync`](../../cbaselib/redis/inc/oi_redis.h#L165) |

```cpp
const int32_t iRet = co_await ::OI::Redis::RedisGet(sKey, stValue);
if (iRet != OI_OK)
{
	co_return iRet;
}
```

> **注意：** 一次只碰一个 key。没有 MGET、MSET，也不会把多条偷偷并成一条。

> **注意：** 返回非 0 时不覆盖原来的输出。先判断返回码，再使用输出。不要把多个 `co_await` 和同一次输出的读写写进一个 `&&` / `||`。

> **注意：** 不存在的标量是 `OI_ERR_NOT_FOUND`。空字符串和空集合仍然成功。超时不等于没执行，INCR、LPUSH 这类写操作不能无条件重试。

> **注意：** [`CallRedisSync`](../../cbaselib/redis/inc/oi_redis.h#L165) 在 poll 回调里会抛 `std::logic_error`。那里改用 `co_await`。

> **注意：** 不要用 `vector<bool>` 当结果。0/1 数组用 `vector<int64_t>`。
