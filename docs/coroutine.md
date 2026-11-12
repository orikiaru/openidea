# 协程

无返回值任务用 `OI::Coro::Task`，有返回值用 `OI::Coro::TaskResult<T>`；RPC 服务端用其别名 `OI::Rpc::RpcTask` 返回 `int32_t` 状态码。单线程和多线程走同一个挂起、唤醒路径，差别在句柄放进哪条队列。

## 怎么写

调用协程函数就开始执行。要结果就 `co_await` 它返回的 Task。想并发，先调用拿到几个 Task，再 `co_await OI::Coro::WhenAll(std::move(vTasks))`：无返回值等到全部结束，有返回值得到按原顺序排列的 `std::vector<T>`。全部结束前不返回；其中一条失败时，其余任务仍跑完，再按序号抛出第一条异常。空列表直接完成。`std::vector<bool>` 是标准库的特化，不是装 `bool` 的普通数组，`operator[]` 返回代理引用，不能拿引用、不能当普通元素用，模板里一实例化就踩坑。因此凡是模板参数为 `bool`、结果却是 `vector<T>` 的接口都必须拦住，禁止返回这个特化。`TaskResult<bool>` 单独等待仍得到 `bool`；交给 `WhenAll` 时得到按原顺序排列的 `vector<int32_t>`，1 为真，0 为假。Redis 的整数回执是 64 位，0/1 标记数组用 `vector<int64_t>`，和原生整数同一条解码，不是因为这个标志位需要 64 位；`int32_t` 装得下 0/1，但更大的整数数组会越界失败。有前后依赖的步骤仍逐个 `co_await`。不要用原子计数加 `yield` 轮询，也不要只为汇合去开 `TaskScope`。`ipcclient` 的 `HoldMgr::Pump` 按这个写法把每个测试项拆成一个返回失败数的函数。带捕获的协程 lambda 和要在独立调用号下运行的任务用 `OI::Coro::StartTask`。让出一段时间用 `co_await OI::Coro::yield(毫秒)`。

- `yield(ms)` 在 `ms > 0` 时进时间轮，到点再跑，等待期间不占 CPU
- `ms <= 0` 只把句柄放回队列，这一轮不再碰它。主循环会先去 poll，再跑新挂上的
- 应用层要空等一段时间，只用 `co_await OI::Coro::yield(毫秒)`。不要 `sleep`、`sleep_for`、`Sleep`、`usleep`。单线程就这一条线程，睡下去 poll 停转，套接字里的数据发不出去

任务创建即运行。时序见 [单线程](sequence-diagram/single-thread/task.md) 和 [多线程](sequence-diagram/multi-thread/task.md)。在协程里调用协程函数，子协程当场在调用方的栈上跑到第一次挂起，再把 Task 返回给调用方；子协程当场跑完，之后的 `co_await` 直接取结果，调用方不挂起。在协程外调用（网络回调、定时器回调、main），子协程只放进运行队列，不在回调线程上跑。子协程停住（等回包或定时）后，`co_await` 把调用方登记到等待列表，子协程结束后逐个 `Schedule` 等待者。

所以不要在第一次挂起之前层层调用子任务，栈会随层数涨。`StartTask` 和 `TaskScope::Start` 总是入队，不加栈。

单线程模式下，任务创建和启动必须在 poll 线程执行；违规时输出警告并抛出 `std::logic_error`，拒绝创建或启动。多线程模式不限制启动线程。故障注入负例可在后台线程刻意触发并验证此拒绝，不作为业务调用范例。

`co_return` 才结束一条 `OI::Coro::Task`。同一个 Task / TaskResult 可以重复或并发 `co_await`，任务只执行一次。两个等待者都在完成前，会挂进 `pWaiters`，结束时逐个 `Schedule`；一个在完成前、一个在完成后，先来的入队，后来的看见 `bReady` 当场取结果；两个都在完成后，都不挂起，各复制一次结果。时序见 [单线程](sequence-diagram/single-thread/waiters.md) 和 [多线程](sequence-diagram/multi-thread/waiters.md)。完成后再等，直接取得已保存的结果或异常。`StartTask` 启动独立任务并返回对应的 Task / TaskResult 实例；可直接 await，也可按值传给多个协程。需要调用号时用 `stTask.GetToken()`。不接返回值仍然正常独立执行，不取消任务。底层 `task.Start()` 只把任务标成独立任务（分配 token、异常直接报告），可重复调用，不会再次执行。

Task 对象、每个等待器及运行中的帧各持一份引用。任务对象析构不会取消已启动任务，等待器可独立保活；最后一个引用释放才销毁帧。任务对象可复制或移动；复制只增加共享引用，不复制任务或结果，已经登记的等待不受影响；移后对象再次 Start/await 抛 logic_error。不支持一边移动/销毁同一个 C++ 对象一边从其他线程访问它。

跨协程共享直接使用 `auto stTask = OI::Coro::StartTask(任务函数);`，后续 `StartTask` 的回调按值捕获 `[stTask]` 并 `co_await stTask`，不需要外套 shared_ptr。任务正常执行，不等等待者到齐：未完成就登记等待，已完成就直接返回保存的结果，不重新入队。等待者数量不限于两个。创建者析构自己的实例不影响其他副本；拷贝/移动赋值和自赋值同样安全。不同副本可以并发使用，同一个实例的赋值/析构与访问仍须由应用同步。

TaskResult 每次等待返回一份结果副本，不移走共享结果。大对象或不可复制对象使用 `TaskResult<std::shared_ptr<T>>`，多个等待者共享同一个对象；其业务数据的并发读写仍由应用同步。异常保留并在每次 await 时重抛。RPC 的输出引用接口本身返回 TaskResult<int32_t>，可以共享等待；不带输出的直连 RPC 等待对象及 Redis/HTTP 请求等待对象仍是单次请求，要共享可以封装到一个 TaskResult 中。

输出引用调用建议独立成句：先 `const int32_t iRet = co_await Call(..., stOutput);`，再判断状态码和读取输出。当前 GCC13 的最小独立协程复现中，同一 `||` 条件内交替执行两次 await、读取并复用同一输出，会错误地判断中间结果；-O0 与 -O3 都能复现，拆成逐条等待后正常。因此不要用一个复杂条件串联依赖前一步输出的多个命令。此限制来自本机编译器验证，不是框架允许并发修改同一输出。Redis 的 0/1 数组使用 `vector<int64_t>`，不把 `vector<bool>` 的代理引用带入协程表达式。

## 阻塞函数

同步文件操作或第三方同步 SDK 等会阻塞线程的函数，可交给 `RunBlocking`，协程等待期间 poll 仍能继续处理事件：

```cpp
int32_t ReadFile(const std::string& sPath, std::string& sData);

const std::string sPath = "data.txt";
std::string sData;
const int32_t iRet = co_await OI::Coro::RunBlocking(ReadFile, sPath, sData);
if (iRet == 0)
{
    UseData(sData);
}
```

直接传入同步函数及其参数；函数必须返回 `int32_t`，接口返回 `TaskResult<int32_t>`。非零状态码原样返回，不自动转换成异常；函数抛出的异常在 `co_await` 时重抛。左值参数借用原对象，包括输出引用；右值参数和可调用对象按值保活。输出对象及其他借用对象必须活到工作结束，丢弃 Task 不会取消工作。同步函数自行决定何时写入输出，失败时输出是否保持原值取决于该函数。同一个任务可重复或并发等待，函数只执行一次，不需要应用管理文件描述符或任务 ID。

阻塞工作按 `OI::Thread::BlockingType` 分类，由各自的固定线程池执行。省略类型时使用 `Default`，默认 4 个线程、1024 个排队项，各类别的在途上限为线程数加队列上限，Default 取该值与 1024 的较小值；在途包含排队、执行中以及完成通知尚未消费的工作，队列上限单独计算且不包含正在执行的工作。其他类型必须在首次使用前调用 `OI::Thread::ConfigureBlockingPool(type, threads, queueLimit)` 显式配置。每个类型第一次启动后再配置返回 `OI_ERR_BUSY`；线程数须为 1–256，队列上限须大于 0。未配置的类型返回 `OI_ERR_NOT_FOUND`，队列已满返回 `OI_ERR_BUSY`，关闭后返回 `OI_ERR_CONNECT`。例如可先配置 `OI::Thread::BlockingType::File`，再用 `co_await OI::Coro::RunBlocking(OI::Thread::BlockingType::File, ReadFile, sPath, sData)` 指定类型；配置返回码须由调用方检查。

工作线程使用项目封装的 `NativeMutex`、`NativeMutexGuard` 和 `NativeMutexCond` 同步，单线程构建中也是真实的线程同步。完成通知使用预分配的内嵌队列节点，通过 poll 的 `PostOnLoop` 无需额外分配地回到事件循环，结果和异常保留在进程内存中，不经过网络传输。阻塞池不使用 `uv_queue_work`，不受 `UV_THREADPOOL_SIZE` 控制。执行顺序和线程亲和性没有保证；后台函数不能访问非线程安全的单线程业务管理器，借用对象的并发访问由应用同步。同步回调及其自有调用链禁止调用 `co_await`、`yield`、`StartTask`、`TaskScope` 和 RPC、Redis、HTTP 异步接口。

框架不提供超时或强制取消；输出引用必须等函数真正完成后才能释放。同步 SDK 可以自行配置超时并返回状态码。全局停止会关闭入口并排空所有分类中已接纳的工作；停止不能从池工作线程调用。应用退出前应先等待任务或 Join 任务作用域，再停止框架，因为全局停止请求后不保证所有业务续体都能运行。

## 挂起

挂起只把句柄放回队列，不在回调里直接 `resume`。RPC 回包和超时也是 `Schedule`。

单次等待走 `OI::Coro::Gate`。`Idle` 还没人认领，`Ready` 是结果已经写好，`Waiting` 是调用方已经挂起。完成方先把结果写进状态，再 `exchange(Ready)`，只有看见 `Waiting` 才 `Schedule`。等待方先记下句柄并提交，再 `exchange(Waiting)`，看见 `Ready` 就不挂起，当场 `await_resume`。时序见 [单线程](sequence-diagram/single-thread/gate.md) 和 [多线程](sequence-diagram/multi-thread/gate.md)。`Task` 的多个等待者仍是完成列表，不走这道门。

时间轮只能在 poll 线程上改。`yield` 经 `OI::Net::Poll::CallOnLoop` 到 poll 线程再 `AddTimer`，到点把句柄放回队列。

## 单线程和多线程

单线程 `PumpOnce` 先 `StScheduler::Poll`，把此刻协程队列整批 `resume`。这一轮里 `yield(0)` 和 `StartTask` 进的是原队列，这次碰不到。然后有待跑协程就 `PollWait(0)`，否则 `PollWait(1000)` 堵住等。

多线程由 `ThreadPool::PostCoroutine` 把句柄放进工人队列。工人和单线程队列都经 `OI::Coro::ResumeTask` 恢复协程，期间新建的任务算作“在协程里”，当场执行。协程在 `final_suspend` 里登记执行引用回收，这次 `resume` 返回后 `DrainDetached` 再释放该引用。当场跑完的子任务在调用方下一次 `co_await` 时回收，不等最外层协程挂起。仍有任务对象或等待者时保留帧与结果。

完成与等待登记使用框架 Mutex，单线程下锁为空操作。等待节点内嵌在父协程帧，不另分配堆节点；唤醒前缓存节点的下一项和父句柄，发布后不再访问可能已经销毁的节点。执行引用保证通知期间子帧仍然有效。

调度和根帧回收钩子由协程运行时自身注册，不依赖应用调用 RPC 或 HTTP。静态链接的纯协程路径用 `python3 tools/check_coro_runtime.py --asan` 验证两种线程模式的 yield、结果返回和闭包释放；业务收发仍使用 ipcclient 的 HoldMgr 回归。

## 服务端

`HandleRpc` 包一层：解包，`co_await` 业务任务，再把回包写出去。请求和回包的包体都以 8 字节调用号开头，客户端按调用号认回包，所以回包不用按接受顺序排队，处理完就发。

```cpp
HandleRpc<AllEchoReq>([](const AllEchoReq& req, AllEchoRes& res) -> OI::Rpc::RpcTask {
    res.stData = req.stData;
    res.iCode = 1;
    co_return 0;
});
```

要在处理里等一段时间，直接 `co_await OI::Coro::yield(毫秒)`，不要占着工人空转。`Hold` 就是这样把回包推迟的。

`StartTask` 按值持有可调用对象直到任务结束，支持带捕获的协程 lambda；`HandleRpc` 也在包装协程中持有处理函数。捕获引用、裸 `this` 不会延长其指向对象的寿命，需由调用方保证有效，或按值捕获 `shared_ptr`。

## 客户端

连接建立后用 `StartTask` 拉起任务，任务里 `co_await RpcCall`。等待键是每次调用新分配的调用号，等待表按调用号分片加锁。回包、超时或发送失败只有一个会把这条协程 `Schedule` 回来。迟到的回包发现调用号已经取走，就丢掉，不会配给下一次调用。

新接口的时长参数是 `OI::Timer::MS`，用 `OI::Timer::Milliseconds(500)`、`Seconds(10)`、`Minutes`、`Hours`、`Days` 构造，默认 `Seconds(180)`。超时统一使用带单位的时长类型，不再接受裸整数秒。由 200 毫秒周期按毫秒分桶扫描，因此 `MS` 不代表毫秒级唤醒精度。非正超时表示不设截止时间，断连仍会结束等待。`yield` 接受毫秒整数，也接受 `MS`。

按服务名调用不传 `sessionid`，见 [服务注册](registry.md)，时序见 [单线程](sequence-diagram/single-thread/route.md) 和 [多线程](sequence-diagram/multi-thread/route.md)。`CallAny` 不带分区，`Call` 打一个分区，`CallAll` 打当前名单上的每一份，`CallMany` 打给出的分区号列表。调用时记下 `RpcDeadline`，查询、建连和回包共用这一份预算，发出去时只剩剩余毫秒。选实例时挂起等注册表回包，不占线程。`CallAll` 和 `CallMany` 先全部发出，再逐个等。这几个接口返回 `TaskResult`，调用时就开始选实例并发请求；同一个对象可以给多个协程等，请求只发一次，大家拿同一份结果。

```cpp
OI::Coro::StartTask([n](OI::Coro::Token) {
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

协程帧只记着 lambda 对象的地址，不复制捕获。直接调用临时的带捕获协程 lambda，lambda 在这一行结束时销毁，第一次挂起之后再读捕获就是悬空访问。带捕获的协程 lambda 必须经 `StartTask` 或 `TaskScope::Start` 启动，它们持有闭包直到任务结束。普通函数和成员函数的参数按值复制进帧，可以直接调用；引用参数要保证对方活到任务结束。

应用层时长参数统一是 `OI::Timer::MS`。`Milliseconds(n)`、`Seconds(n)`、`Minutes(n)`、`Hours(n)`、`Days(n)` 在构造时换成毫秒，可以直接传给 `yield` 和 RPC。

`Days(n)` 是每单位 24 小时的持续时长；日历跨天、跨周等刷新判断仍使用时间辅助接口。

## 和 C# 的对应

应用层就认这几样，名字先沿用现在的，不另起一套。

| C# | 这边 |
| --- | --- |
| `Task` / `Task<T>` | `OI::Coro::Task` / `TaskResult<T>` |
| `await` | `co_await` |
| `Task.WhenAll` | `OI::Coro::WhenAll` |
| `Task.Run` | `OI::Coro::StartTask` |
| `Task.Yield` | `co_await OI::Coro::yield(0)` |
| `CancellationToken` | `OI::Coro::StopToken`，任务自己看 `StopRequested()` |
| `TaskCompletionSource` | 完成门 `OI::Coro::Gate`。结果先写好，再按门的状态决定要不要把已经挂起的人叫醒。RPC、Redis、HTTP 的单次等待用它 |
| 一组后台任务，统一取消再 `WhenAll` | `OI::Coro::TaskScope` |

`TaskScope` 不是 `TaskCompletionSource`。它只负责圈住一组任务：`Start` 放进去，`RequestStop` 给 `StopToken`，`Join` 等它们结束。只取消某一段等待时，传 `StopToken` 即可，不必建作用域。

线程池只调度协程句柄。`PostCoroutine`、`PumpOnce`、`SetDrain`、`SetPending`、`SetResume` 只有运行时用，应用层不调用。

## 异常和任务作用域

子任务的异常在父任务 `co_await` 处重新抛出，可以用 try/catch 捕获。独立根任务异常默认打印到 stderr，也可用 `SetTaskErrorHandler` 设置线程安全的错误处理函数。协程任务的异常如果直到最后一个引用释放都没有被 `co_await` 取走，也交给同一个处理函数，不会悄悄丢掉。

`TaskScope` 持有一组任务；`scope.Start(fn)` 的 fn 接收 `(OI::Coro::Token, OI::Coro::StopToken)`，循环检查 `StopRequested()`。`RequestStop()` 关闭入口并请求协作停止，`co_await scope.Join()` 等待所有任务完成后转交第一个异常。Join 本身也关闭入口。模块销毁前必须先完成 Join，再停止框架线程池。析构只请求停止，不能代替异步 Join。

停止不强拆正在等待的 RPC、Redis 或定时器：它们正常完成或超时后任务检查停止标记；因此作用域任务需要设置有限超时，避免无限等待。捕获裸 this 的生命周期由模块 Join 保证。

路由连接在逻辑断连时立即撤销旧会话；下一次调用可直接发起重连并排队，不需要应用轮询会话状态。路由已连接发送和建连后补发统一区分错误：发送预算不足返回 `OI_ERR_BUSY`，会话不存在或已关闭返回 `OI_ERR_CONNECT`。背压不会主动关闭连接。

首次连接前的 RPC 默认最多等待 65536 条、64MiB；可通过 `SetPendingCallLimits(count, bytes)` 调整，0 表示不允许排队。达到任意限额返回 `OI_ERR_BUSY`，超时删除通过调用号索引完成。

```cpp
OI::Coro::TaskScope tasks;
tasks.Start([](OI::Coro::Token token, OI::Coro::StopToken stop) -> OI::Coro::Task {
    while (!stop.StopRequested()) {
        // 一轮业务；网络调用必须给出有限超时。
        co_await OI::Coro::yield(OI::Timer::Milliseconds(100));
    }
});
// 模块退出协程内，先关闭任务，再销毁业务数据。
tasks.RequestStop();
try { co_await tasks.Join(); }
catch (const std::exception& e) { /* 记录错误，任务已经全部结束 */ }
```

RPC 登记成功后的组包、排队或发送异常，先通过原完成机制结束等待，再在 `co_await` 处重新抛出；即使超时/断连已取走等待项，也要等该回调完成，不能直接销毁等待对象。登记本身失败会在分片锁内回滚。

RPC 超时桶按单调时钟绝对截止时间所在的 200 毫秒刻度分桶，扫描遇到未来刻度即停止，不遍历后面的长超时桶。当前刻度内逐项校验精确截止时间，避免乱序登记挡住已到期请求，不提前触发，也不额外增加一个取整周期；实际唤醒仍受扫描和调度延迟影响。回包、限流、发送失败、主动结束或断连都会通过链表索引直接删除超时记录，空桶也回收。`GetRpcWaitStats()` 提供等待项、超时记录和桶数量的诊断快照，逐分片加锁，有并发变化时不是全局同一时刻快照。

## 简化客户端调用

单次调用推荐由调用方提供响应，返回 `OI::Coro::TaskResult<int32_t>`，与服务端的 `RpcTask` 是同一种任务类型。请求由任务按值持有，响应类型由请求自动关联，传错响应类型会编译失败：

```cpp
OI::RpcData::HoldRes stResponse;
const int32_t iRet = co_await stClient.Call(stRequest, stResponse, OI::Timer::Seconds(3));
if (iRet != 0)
{
    co_return iRet;
}
// 此处直接使用 stResponse。
```

`client.Call(req, res, duration)` 使用这个客户端的连接；`client.Call(zone, req, res, duration)` 指定分区；`client.CallAny(req, res, duration)` 按服务名选择实例。自由函数 `RpcCall(client, req, res, duration)` 同样返回任务；显式会话用 `RpcCall(manager, sid, req, res, duration)`，服务字符串用 `RpcCall("gameserver.1001", req, res, duration)`。这些接口都不需要显式响应模板参数。命名接口从入口保存单调时钟截止时间，注册查询、连接前排队和回包共用剩余预算；预算用尽后不再发送请求。注册查询另有最多 1 秒的故障检测上限；非正 RPC 时长不限制回包等待，但不取消注册查询的故障检测。实际唤醒仍受时间轮/调度粒度影响。

可以先保存任务、后等待，也可以将任务按值交给多个协程等待：请求只发一次、响应只写一次，重复等待返回保存的状态码，不会再次修改响应。协程内跨挂起点使用的局部响应在协程帧中存活。响应和管理器必须覆盖任务的执行期，**丢弃任务对象不会取消请求**，因此输出引用版必须等待完成再离开响应作用域；不能把同一响应交给多个独立请求并发写。请求创建后可修改或销毁原值；大请求可以 `std::move` 移交。

回包在任务恢复后解码到临时响应，完整解码成功才移动赋值到调用方响应；业务错误、超时、断连或解码失败保留原值。任务完成前先结束底层回调保活，迟到回包不会再写输出。自定义响应的移动赋值若可能抛异常，其异常安全由该类型负责。大结果不经过 `RpcResult` 中转，也不因多个任务等待者而重复复制响应。输出任务完成（包括错误或异常）时销毁其持有的请求；继续保存任务用于重复等待，不会保留请求中的大块资源。

底层 `MakeRpcCallAwaiter` 仅供框架和故障注入用例使用，借用请求与响应，只能等待一次；应用层使用上述任务接口。已有自带结果接口继续保留，适合独立任务或不方便管理输出引用的调用：

生成器为请求生成响应类型关联，并让 `rpc_ids.h` 包含对应的响应定义。推荐写法：

```cpp
auto result = co_await OI::Rpc::RpcCall(client, req, OI::Timer::Seconds(3));
if (result.GetRet() != 0) { co_return; }
auto& res = result.GetRes(); // 自动配对的响应类型
```

不带输出的直连接口在实际 await 提交时选择当前会话，并在同一客户端临界区决定排队或绑定回包来源；构造等待对象不会截取会话号，首次连接前仍按原规则排队。可以先创建等待对象，等连接成功后再 await。需要固定会话时使用 `RpcCall(manager, sid, req, duration)`，此接口不会自动改发到新连接。自带结果的显式响应模板接口保留；输出引用的直接 co_await 写法兼容，但返回类型已改为 TaskResult<int32_t>，需要操作底层等待器的框架代码改用 MakeRpcCallAwaiter。新接口只接受明确的时长类型。创建等待对象时复制请求，调用方可用 `std::move(req)` 交出大请求以避免复制。未使用的任务或 RPC 等待对象会产生 nodiscard 编译诊断。

`CallRedisSync`、`RegistryClient::Lookup`、`PullSync`、`Publish` 不能在 poll 回调（含定时器回调）里调。适合在启动阶段调用，运行期用 `co_await` 版本。注册表的 `Lookup`、`PullSync`、`Publish` 在 poll 回调里需要真正等待时会抛 `logic_error`，协程里用 `PullAsync`。在协程里直接调这些同步接口：多线程会占住一个工人；单线程的 `CallRedisSync` 在当前协程里转 `PumpOnce`，注册表这几个在单线程里转 `PollWait`。只建议测试或特殊场景使用。

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

服务端推荐只接收 `(req, res)`，注册只写请求类型 `HandleRpc<Req>`，响应类型自动配对。需要连接信息时接收 `(const RpcContext&, req, res)`，上下文包含 `iToken`、`stManager` 引用和 `iSessionId`，仅在本次处理期间有效。协程处理函数返回 `OI::Rpc::RpcTask`，用 `co_return int32_t` 返回状态码；同步处理函数直接返回 `int32_t`。不再接受旧五参数回调或无返回值的 `OI::Coro::Task` 处理函数：

```cpp
HandleRpc<HoldReq>([](const HoldReq& req, HoldRes& res) -> OI::Rpc::RpcTask {
    if (req.iDelayMilliseconds < 0) {
        co_return 1001;
    }
    res.iMark = req.iMark;
    co_return 0;
});
```

`RpcTask` 是 `OI::Coro::TaskResult<int32_t>` 的别名，与无返回值 `OI::Coro::Task` 共用同一套帧、调度和完成门，不增加包装层。带返回值的任务也支持嵌套 `co_await`、`StartTask` 和异常传播。请求解码异常或处理函数异常返回 `OI_ERR_FAIL`，处理函数异常还会交给任务错误处理器。等待登记/发送时的本地 C++ 异常仍按原约定从 `co_await` 抛出。

## 应用层推荐：命名函数 + 宏注册

协议名与生成类型按 `NameReq / NameRes` 对应。宏隐藏框架名字空间和固定参数类型，业务函数名自由指定，注册处只注入函数名：

```cpp
static OI_RPC_HANDLER(Hold, HandleHold)
{
    if (stRequest.iDelayMilliseconds < 0) co_return 1001;
    if (stRequest.iDelayMilliseconds > 0) co_await OI::Coro::yield(stRequest.iDelayMilliseconds);
    stResponse.iMark = stRequest.iMark;
    co_return 0;
}

// 组件初始化时注册一次。
OI_RPC_BIND(Hold, HandleHold);
```

`OI_RPC_HANDLER(Name, Function)` 用于协程函数声明或定义，提供 `stRequest`（const 引用）和 `stResponse` 参数，函数体用 `co_return int32_t`。`OI_RPC_SYNC_HANDLER` 用于同步处理，函数体用 `return int32_t`。`OI_RPC_CONTEXT_HANDLER` 额外提供 `stContext`，按需访问 iToken、stManager 和 iSessionId。头文件声明可写 `OI_RPC_HANDLER(Hold, HandleHold);`。宏本身不加 static，文件内部函数可自行加 static。

`OI_RPC_BIND(Name, Function)` 使用请求关联的响应类型注册，不增加运行时包装层。也可传函数对象或 lambda。自由函数不需要对象。实例成员函数用 `OI_RPC_BIND_SELF(Name, Type, Member)`：处理时从 `stContext.stManager` 还原成 `Type` 再调用，不在注册时绑 `this`。函数体里要自己拿对象时用 `OI_RPC_SELF(Type)`。对象必须活到所有请求处理结束。原模板接口仍可直接使用，不按 NameReq/NameRes 命名的协议可用模板注册。

## 单向消息

调用号 0 是单向消息：不登记等待、不回包。管理器上用 `Notify(sid, msg)`，这次请求的上下文上用 `stContext.Notify(msg)`，换一条会话用 `stContext.Notify(sid, msg)`。返回 `SendBytesResult` 的错误码，编码失败是 `OI_ERR_ENCODE`。UDP 先用 `OpenPeer(ip, port)` 换成 sid。接收和 RPC 共用一张按协议号的处理表，一个协议号只注册一种：

```cpp
static OI_NOTIFY_HANDLER(UdpNote, HandleNote)
{
    co_return stContext.Notify(stRequest);
}

OI_NOTIFY_BIND(UdpNote, HandleNote);
```

`OI_NOTIFY_HANDLER(Name, Function)` 提供 `stContext` 和 `stRequest`。成员函数用 `OI_NOTIFY_BIND_SELF(Name, Type, Member)`，规则与 `OI_RPC_BIND_SELF` 相同。生成器要求每条 rpc 都写 response。调用号为 0 时处理返回码不发回对端，服务端执行额度满时直接丢弃，不回 `OI_ERR_BUSY`。

`ProtoMultiClientManager` 的 `NotifyPeer` 和 `NotifyAny` 复制消息并启动异步 RPC 任务，复用协议号和连接的暂存、重连路径，默认超时 180 秒。返回 `OI_OK` 只表示任务已启动，发送、回执或超时失败由任务错误处理器报告。接收端需使用带 RPC 适配的新版 `HandleNotify`：对非零调用号回传处理返回码，成功时附带默认构造的关联 response；执行额度满时返回 `OI_ERR_BUSY`。旧版只处理调用号 0 的接收端不会为包装通知回包，发送任务会等待到超时。

定时器回调和停止回调抛异常时，交给 `OI::Thread::SetTaskErrorHandler`，默认输出 stderr。处理器自身抛异常也不会穿透调用方。服务端响应编码失败会丢弃半包，重新发送 `OI_ERR_FAIL` 的无响应体回执；无法编码或发送失败回执时关闭连接，避免调用方只靠超时发现错误。

## 等待建连

`StartClient()` 统一为立即启动、可等待的接口，不再接受布尔参数：

```cpp
client.StartClient(); // 不等待：调用时就发起非阻塞建连，忽略临时对象不会取消连接。
if (!(co_await client.StartClient())) co_return; // 等建连成功，再处理后续逻辑。
```

已有连接时返回成功；并发调用等待同一代连接，不重复创建 socket。失败、服务地址切换、停止入口后返回 false。等待只挂起协程，完成排回现有调度队列，不直接 resume。可以先 `auto connecting = client.StartClient();`，之后再 `co_await connecting`；即便连接先完成也不会丢唤醒。返回对象仅可移动，不可复制；同一对象支持多个协程同时 await；完成后也可再次 await，所有等待者读取同一代连接结果。等待已移走的对象会抛出 std::logic_error。

管理器必须覆盖网络操作和等待者的生命周期；全局停止前仍按 TaskScope 约定 Join。接口不提供单独连接超时参数，失败时间由网络连接结果决定。旧 `StartClient(false)` 同步入口和独立 `Connect()` 入口已移除，应用等待建连统一使用 `co_await StartClient()`。

建连等待在启动 socket 前登记；完成时先摘取该代等待者，再执行连接成功回调。成功回调内关闭旧连接并再次 StartClient，新一代不会收到旧连接的成功结果。HoldMgr 覆盖该重入场景，要求首次 true、下一次连接拒绝 false。

## 请求消费与服务端容量

不带输出的直连 RPC 请求等待对象只能 await 一次；重复、并发等待及等待已移走的对象会在修改句柄或提交之前抛出 `std::logic_error`。已提交的底层等待对象也不能再移动。输出引用版返回 TaskResult<int32_t>，同一任务可以重复/并发等待，但不重新发送。需要重新发送就重新创建调用。显式 sid 的重载始终固定会话，首次连接前也不排队；只有自动选会话的重载可以等待首次建连。

服务端在创建处理协程之前检查 `[Rpc]` 的进程总量和每条会话额度：`max_inflight_count` 默认 65536，`max_inflight_bytes` 默认 67108864，`session_max_inflight_count` 默认 4096，`session_max_inflight_bytes` 默认 16777216。各项必须为正整数。字节统计收到的请求包体（含调用号），不等于整个进程 RSS；业务生成的响应、外部资源和网络输入缓冲另计。

超限立即回标准 `OI_ERR_BUSY`，不创建业务协程；错误回执无法发送时关闭会话。额度由原有根协程闭包中的可移动 RAII 对象持有，额度凭证不新增包装协程或单独堆分配；正常完成、业务异常、响应编码失败都自动归还。断连不提前释放仍在执行的业务额度，避免借助重连绕过进程限额。业务端收到 Busy 后自行决定退避，框架不自动重放写请求。

`HoldMgr` 保留共享建连、请求重复/并发/移后使用、首次连接前固定 sid 的回归。`python3 tools/run_regressions.py --case rpc-admission-count` 和 `rpc-admission-bytes` 用临时限额验证多连接过载、标准 Busy 回执、额度恢复与错误路径；均已纳入 `--case all`。


### 直连发送错误与回包诊断

直连客户端、显式 sid 调用及首次连接后的待发请求补发，均保留底层发送错误码：发送预算不足返回 `OI_ERR_BUSY`，会话不存在或已关闭返回 `OI_ERR_CONNECT`，不再用通用 FAIL 覆盖。BUSY 不关闭健康连接；应用可自行限流，框架不自动重放写请求。

客户端回包解码失败保持原输出，返回 `OI_ERR_FAIL`，并记录 `rpc failure stage=decode_response`、调用号、协议号和原始解析异常。sid 来自该回包实际匹配的等待项，不重新查询可能已经重连的管理器；协程 token 在挂起时读取（框架任务按其状态锁读取，其他协程类型记为 0）。内部 BindRpcCall 回包回调现接收 `(BytesStream&, sid_t)`，应用层 RpcCall 接口不变。
