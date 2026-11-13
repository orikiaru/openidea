# openidea

C++20 网络进程框架。业务写协议和协程，收发、回包配对、超时、重连由框架做完。同一条协程可以跑在单线程，也可以跑在多线程。

## 能做什么

| 功能 | 调用 |
| --- | --- |
| 请求回包 | `co_await RpcCall`，处理函数 `co_return` 错误码 |
| 单向消息 | `Notify` |
| UDP | `OpenPeer`，业务仍走会话和 RPC |
| Redis | `co_await RedisGet` / `RedisSet`，一个 key 一次 |
| HTTP | `co_await Client::Get` |
| 数据库 | 连接池、参数绑定、事务 |
| 定时 | `AddTimer`，空等 `co_await yield` |
| 服务名 | 配置里写 `service`，按名连上每一台 |
| 日志 | `SendLog`，日志服没连上时先攒着 |

接口和注意点见 [基础库接入表](docs/cbaselib.md#接入)。

## 一条调用链

回声走的就是这条。服务端在 `modules/ipcserver/src/ipcserver.cpp`，客户端在 `modules/ipcclient/src/echo_mgr.cpp`。

1. `rpc.proto` 写请求、回复和一条 `rpc`。字段号手写，生成器不补号。
2. 进程 `StartFramework` 读 toml，拉起 poll、定时器和协程。
3. 组件 `Init` 里 `OI_RPC_BIND` 或 `OI_RPC_BIND_SELF` 注册处理函数。
4. 请求进来，框架按协议号交给处理函数。函数填回复，返回 `0`。失败只回错误码，不带回复体。
5. 客户端把回复对象传进 `RpcCall` 或 `CallAny`，`co_await` 得到错误码。返回码是 `0` 再读回复。非 `0` 时回复保持调用前的值。
6. 这一次调用按调用号认回包。超时、断线、对端忙都是明确的错误码，框架不自动重放。

```cpp
static OI_RPC_SYNC_HANDLER(AllEcho, HandleAllEcho)
{
    stResponse.stData = stRequest.stData;
    stResponse.iCode = 1;
    return 0;
}

// Init
OI_RPC_BIND(AllEcho, HandleAllEcho);
```

```cpp
::OI::RpcData::AllEchoReq stRequest;
::OI::RpcData::AllEchoRes stResponse;
const int32_t iRet = co_await IpcClient::GetInstance().CallAny(
    stRequest, stResponse, ::OI::Timer::Seconds(3));
if (iRet != OI_OK)
{
    co_return iRet;
}
```

要自己加一条，从 [加一个回声协议](docs/module.md#加一个回声协议) 照着写：`rpc.proto` 里加请求、回复和 `rpc`，服务端 `Init` 里注册，客户端连上后 `co_await`。服务端处理函数和客户端调用的完整写法在 [协程](docs/coroutine.md) 的服务端、客户端两节。各功能接到哪个函数，看 [接入表](docs/cbaselib.md#接入)。

## 性能

线上整数和浮点统一小端。解码用生成器的第一个参数区分，原因见 [字段分发](docs/pb-dispatch.md)。

```shell
python tools/gen_dispatch_bench.py array
python tools/gen_dispatch_bench.py switch
```

`array` 是宏字段表，编译快。`switch` 解码大约快 1 微秒，编译大约慢到 6 倍。这次是 1000 条协议、每条 128 个 `int32_t` 字段。机器是 4 核 Intel Xeon Platinum 8255C，标称 2.50 GHz，实测 2494 MHz。

同一台机器上，回声 100 万次，`ok=1000000 mismatch=0 rpc_fail=0`，用时 12.9 秒，约 7.7 万次/秒。协程压力测 10 万条同时挂起的内存，以及 `StartTask` 到跑完能跑多少条/秒：挂起路径（创建挂起再唤醒）和空任务路径分开记；数字以 [最近一次测试](docs/lasttesting.md) 为准。这两项都不进默认回归，要测时再单独开。

## 说明

- [基础库](docs/cbaselib.md)
- [协程](docs/coroutine.md)
- [模块和协议](docs/module.md)
- [数据库](docs/database.md)
- [服务注册](docs/registry.md)
- [定时器](docs/timer.md)
- [开发标准](docs/develop.md)
