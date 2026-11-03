# 协程

无返回值任务用 `coro::Task`，有返回值用 `coro::TaskResult<T>`；RPC 服务端用其别名 `OI::Rpc::RpcTask` 返回 `int32_t` 状态码。单线程和多线程走同一个挂起、唤醒路径，差别在句柄放进哪条队列。

## 怎么写

要结果就 `co_await`，不等就 `coro::StartTask`。让出一段时间用 `co_await coro::yield(毫秒)`。

- `yield(ms)` 在 `ms > 0` 时进时间轮，到点再跑，等待期间不占 CPU
- `ms <= 0` 只把句柄放回队列，这一轮不再碰它。主循环会先去 poll，再跑新挂上的
- 应用层要空等一段时间，只用 `co_await coro::yield(毫秒)`。不要 `sleep`、`sleep_for`、`Sleep`、`usleep`。单线程就这一条线程，睡下去 poll 停转，套接字里的数据发不出去

`co_await` 一条 `Task` 会在调用方的栈上把子协程 `resume` 到它自己挂起为止。子协程当场跑完，调用方不挂起。子协程自己停住（等回包或定时）时，把调用方记在 `stContinuation` 上，子协程结束再 `Schedule` 调用方。

所以不要在第一次挂起之前层层 `co_await` 子任务，栈会随层数涨。`StartTask` 只入队，不加栈。

`co_return` 才结束一条 `coro::Task`。`task.Start()` 或 `StartTask` 把句柄交给调度器。调用方手里的临时 `Task` 析构不会释放正在跑的帧。

## 挂起

挂起只把句柄放回队列，不在回调里直接 `resume`。RPC 回包和超时也是 `Schedule`。

时间轮只能在 poll 线程上改。`yield` 经 `OI::Net::Poll::CallOnLoop` 到 poll 线程再 `AddTimer`，到点把句柄放回队列。

## 单线程和多线程

单线程主循环每一轮只跑进入时已经在队列里的普通 `Task`，再 `StScheduler::Poll` 只 `resume` 进入时已经在协程队列里的句柄。这一轮里 `yield(0)` 和 `StartTask` 进的是原队列，这次碰不到。然后如果还有待跑的任务或协程，`PollWait(0)` 只试一次网络；什么都没有才 `PollWait(1000)` 堵住等。

多线程由 `ThreadPool::PostCoroutine` 把句柄放进工人队列。工人只 `resume`，不在工人里销毁协程帧。根协程在 `final_suspend` 里 `DetachFinished`，`resume` 返回后 `DrainDetached` 再销毁。

## 服务端

`HandleRpc` 包一层：解包，`co_await` 业务任务，再把回包写出去。请求和回包的包体都以 8 字节调用号开头，客户端按调用号认回包，所以回包不用按接受顺序排队，处理完就发。

```cpp
HandleRpc<AllEchoReq>([](const AllEchoReq& req, AllEchoRes& res) -> OI::Rpc::RpcTask {
    res.stData = req.stData;
    res.iCode = 1;
    co_return 0;
});
```

要在处理里等一段时间，直接 `co_await coro::yield(毫秒)`，不要占着工人空转。`Hold` 就是这样把回包推迟的。

`StartTask` 按值持有可调用对象直到任务结束，支持带捕获的协程 lambda；`HandleRpc` 也在包装协程中持有处理函数。捕获引用、裸 `this` 不会延长其指向对象的寿命，需由调用方保证有效，或按值捕获 `shared_ptr`。

## 客户端

连接建立后用 `StartTask` 拉起任务，任务里 `co_await RpcCall`。等待键是每次调用新分配的调用号，等待表按调用号分片加锁。回包、超时或发送失败只有一个会把这条协程 `Schedule` 回来。迟到的回包发现调用号已经取走，就丢掉，不会配给下一次调用。

整数超时保持以秒为单位，默认 180；也可传 `std::chrono::milliseconds`，例如 `OI::Timer::Milliseconds(500)`、`OI::Timer::Seconds(10)`。由 200 毫秒周期扫描，因此毫秒参数不代表毫秒级唤醒精度。非正超时表示不设截止时间，断连仍会结束等待。`yield` 同样支持时长参数。

```cpp
coro::StartTask([n](coro::Token) {
    return RunOne(n);
});

int ret = co_await RpcCall(mgr, sid, req, res);
```

`ret == OI_OK` 是服务端返回成功且响应已解出，`OI_ERR_TIMEOUT` 是超时，`OI_ERR_CONNECT` 是连接失败或连接断开，`OI_ERR_FAIL` 是发送失败或解包失败。这条会话还没建立过时，调用先挂起，后面的调用按顺序堆着；连上后补发。连接失败，或在连上之前超时，直接返回 `OI_ERR_CONNECT`。已经连上之后的超时仍是 `OI_ERR_TIMEOUT`。等待绑定管理器、会话和回复协议号；其他连接的回包不会消费等待项。断连时框架自动结束该连接的 RPC，不需要业务额外清理。

新代码可直接取得结果，请求由等待对象按值持有：

```cpp
auto result = co_await RpcCall<AllEchoRes>(mgr, sid, req, OI::Timer::Seconds(10));
if (result.GetRet() != 0) { co_return; }
auto& res = result.GetRes();
```

直接 await 普通 `Task` 不受 `StartTask` 闭包保活保护；不要直接 await 临时的带捕获协程 lambda。

应用层时间统一由 `oi_duration.h` 构造：`OI::Timer::Milliseconds(n)`、`Seconds(n)`、`Minutes(n)`、`Hours(n)`、`Days(n)`。这些函数保留时长类型，可以直接传给 `yield` 和 RPC，无需引入字面量名字空间。

`Days(n)` 是每单位 24 小时的持续时长；日历跨天、跨周等刷新判断仍使用时间辅助接口。

## 异常和任务作用域

子任务的异常在父任务 `co_await` 处重新抛出，可以用 try/catch 捕获。独立根任务异常默认打印到 stderr，也可用 `SetTaskErrorHandler` 设置线程安全的错误处理函数。

`TaskScope` 持有一组任务；`scope.Start(fn)` 的 fn 接收 `(coro::Token, coro::StopToken)`，循环检查 `StopRequested()`。`RequestStop()` 关闭入口并请求协作停止，`co_await scope.Join()` 等待所有任务完成后转交第一个异常。Join 本身也关闭入口。模块销毁前必须先完成 Join，再停止框架线程池。析构只请求停止，不能代替异步 Join。

停止不强拆正在等待的 RPC、Redis 或定时器：它们正常完成或超时后任务检查停止标记；因此作用域任务需要设置有限超时，避免无限等待。捕获裸 this 的生命周期由模块 Join 保证。

首次连接前的 RPC 默认最多等待 65536 条、64MiB；可通过 `SetPendingCallLimits(count, bytes)` 调整，0 表示不允许排队。达到任意限额返回 `OI_ERR_BUSY`，超时删除通过调用号索引完成。

```cpp
coro::TaskScope tasks;
tasks.Start([](coro::Token token, coro::StopToken stop) -> coro::Task {
    while (!stop.StopRequested()) {
        // 一轮业务；网络调用必须给出有限超时。
        co_await coro::yield(OI::Timer::Milliseconds(100));
    }
});
// 模块退出协程内，先关闭任务，再销毁业务数据。
tasks.RequestStop();
try { co_await tasks.Join(); }
catch (const std::exception& e) { /* 记录错误，任务已经全部结束 */ }
```

RPC 登记成功后的组包、排队或发送异常，先通过原完成机制结束等待，再在 `co_await` 处重新抛出；即使超时/断连已取走等待项，也要等该回调完成，不能直接销毁等待对象。登记本身失败会在分片锁内回滚。

RPC 超时桶采用可直接摘除的链表节点。回包、限流、发送失败、主动结束或断连都会同时删除超时记录，空桶也回收；长超时请求不会再阻挡后续已完成请求的记录回收。`GetRpcWaitStats()` 提供等待项、超时记录和桶数量的诊断快照，逐分片加锁，有并发变化时不是全局同一时刻快照。

## 简化客户端调用

生成器为请求生成响应类型关联，并让 `rpc_ids.h` 包含对应的响应定义。推荐写法：

```cpp
auto result = co_await OI::Rpc::RpcCall(client, req, OI::Timer::Seconds(3));
if (result.GetRet() != 0) { co_return; }
auto& res = result.GetRes(); // 自动配对的响应类型
```

连接管理器在调用时取当前会话号，首次连接前仍按原规则排队。需要指定会话时使用 `RpcCall(manager, sid, req, duration)`。旧的显式响应模板、输出引用接口均保留；新接口只接受明确的时长类型。创建等待对象时复制请求，调用方可用 `std::move(req)` 交出大请求以避免复制。未使用的任务或 RPC 等待对象会产生 nodiscard 编译诊断。

多线程工作线程在同一优先级普通任务等待时，最多连续执行 32 次协程恢复后取一次普通任务，防止持续 yield(0) 的协程饿死普通任务。单线程同步适配器与主循环共用 PumpOnce，推进普通任务、协程和 IO；不要从网络回调中使用同步适配器。

## 标准 RPC 回执

请求包体为 `uint64_t call_id + Req`，回执为 `uint64_t call_id + int32_t retcode`，只有 `retcode == 0` 才跟随序列化的 `Res`。这是线格式变更，客户端和服务端必须一起升级，不能与旧版混用。

`0` 表示成功，负数保留给框架错误（超时、断连、发送/解码失败等），正数用于业务错误。客户端先读取状态码，非零时不解析响应体。输出引用只在成功解包后更新，任何错误都保留调用方原值。

两种客户端接口共用相同回执和完成机制：

```cpp
auto result = co_await RpcCall(client, req);
if (result.GetRet() != 0) { co_return; }
auto& res = result.GetRes();

// 或者由调用方提供响应对象：
Response output;
int32_t ret = co_await RpcCall(client, req, output);
```

`GetRet()` 返回 `int32_t`。`GetRes()` 在成功时返回引用，结果对象必须仍然存活；也可 `auto res = result.GetRes()` 复制响应，或 `auto res = std::move(result).GetRes()` 转移响应。错误时访问响应抛出 `std::logic_error`，不会把默认响应伪装成成功。结果字段通过这两个方法访问。

服务端推荐只接收 `(req, res)`，注册只写请求类型 `HandleRpc<Req>`，响应类型自动配对。需要连接信息时接收 `(const RpcContext&, req, res)`，上下文包含 `iToken`、`stManager` 引用和 `iSessionId`，仅在本次处理期间有效。旧五参数 `(token, mgr, sid, req, res)` 继续兼容。协程处理函数返回 `OI::Rpc::RpcTask`，用 `co_return int32_t` 返回状态码；同步处理函数直接返回 `int32_t`。原来的 `coro::Task` 处理函数继续兼容，正常结束视为成功：

```cpp
HandleRpc<HoldReq>([](const HoldReq& req, HoldRes& res) -> OI::Rpc::RpcTask {
    if (req.iDelayMilliseconds < 0) {
        co_return 1001;
    }
    res.iMark = req.iMark;
    co_return 0;
});
```

`RpcTask` 是 `coro::TaskResult<int32_t>` 的别名，与无返回值 `coro::Task` 共用同一套帧、调度和完成门，不增加包装层。带返回值的任务也支持嵌套 `co_await`、`StartTask` 和异常传播。请求解码异常或处理函数异常返回 `OI_ERR_FAIL`，处理函数异常还会交给任务错误处理器。等待登记/发送时的本地 C++ 异常仍按原约定从 `co_await` 抛出。

## 应用层推荐：命名函数 + 宏注册

协议名与生成类型按 `NameReq / NameRes` 对应。宏隐藏框架名字空间和固定参数类型，业务函数名自由指定，注册处只注入函数名：

```cpp
static OI_RPC_HANDLER(Hold, HandleHold)
{
    if (stRequest.iDelayMilliseconds < 0) co_return 1001;
    if (stRequest.iDelayMilliseconds > 0) co_await coro::yield(stRequest.iDelayMilliseconds);
    stResponse.iMark = stRequest.iMark;
    co_return 0;
}

// 组件初始化时注册一次。
OI_RPC_BIND(Hold, HandleHold);
```

`OI_RPC_HANDLER(Name, Function)` 用于协程函数声明或定义，提供 `stRequest`（const 引用）和 `stResponse` 参数，函数体用 `co_return int32_t`。`OI_RPC_SYNC_HANDLER` 用于同步处理，函数体用 `return int32_t`。`OI_RPC_CONTEXT_HANDLER` 额外提供 `stContext`，按需访问 iToken、stManager 和 iSessionId。头文件声明可写 `OI_RPC_HANDLER(Hold, HandleHold);`。宏本身不加 static，文件内部函数可自行加 static。

`OI_RPC_BIND(Name, Function)` 使用请求关联的响应类型注册，不增加运行时包装层。也可传函数对象或 lambda；实例成员函数需绑定对象，且对象必须活到所有请求处理结束。原模板接口仍可直接使用，不按 NameReq/NameRes 命名的协议可用模板注册。

普通任务回调抛异常时，闭包仍会释放，调度器捕获并报告异常后继续处理后续任务；可通过 `OI::Thread::SetTaskErrorHandler` 设置线程安全处理器，默认输出 stderr。处理器自身抛异常也不会穿透工作线程。服务端响应编码失败会丢弃半包，重新发送 `OI_ERR_FAIL` 的无响应体回执；无法编码或发送失败回执时关闭连接，避免调用方只靠超时发现错误。

## 等待建连

`StartClient()` 统一为立即启动、可等待的接口，不再接受布尔参数：

```cpp
client.StartClient(); // 不等待：调用时就发起非阻塞建连，忽略临时对象不会取消连接。
if (!(co_await client.StartClient())) co_return; // 等建连成功，再处理后续逻辑。
```

已有连接时返回成功；并发调用等待同一代连接，不重复创建 socket。失败、服务地址切换、停止入口后返回 false。等待只挂起协程，完成排回现有调度队列，不直接 resume。可以先 `auto connecting = client.StartClient();`，之后再 `co_await connecting`；即便连接先完成也不会丢唤醒。返回对象仅可移动，不可复制；同一对象完成后可再次 await，但不能由多个协程同时 await 同一个对象（并发调用分别使用 StartClient 返回的对象）。

管理器必须覆盖网络操作和等待者的生命周期；全局停止前仍按 TaskScope 约定 Join。接口不提供单独连接超时参数，失败时间由网络连接结果决定。旧 `StartClient(false)` 同步入口和独立 `Connect()` 入口已移除，应用等待建连统一使用 `co_await StartClient()`。

建连等待在启动 socket 前登记；完成时先摘取该代等待者，再执行连接成功回调。成功回调内关闭旧连接并再次 StartClient，新一代不会收到旧连接的成功结果。HoldMgr 覆盖该重入场景，要求首次 true、下一次连接拒绝 false。
