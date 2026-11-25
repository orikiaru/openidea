# 服务注册

业务进程只要两样东西：一份连接名单，以及自己要不要上报地址。静态还是动态只配在 registry 进程上。

没有 `Registry.ip` 时用本地名单。`[Registry] instances` 一行一个：`名字 实例号 ip 端口`，分区服再跟分区号。同名同实例号重复、格式不对，启动失败。`CallAll` 按这份名单每一台打一份。名单里的实例一直算在线，挂了靠建连失败暴露。本进程不在名单里，或地址和对外地址不一致，打错误日志，不拒绝启动，也不上报。

```toml
[Registry]
instances = [
    "gameserver 1001 10.0.0.1 17981 1001",
    "gameserver 1002 10.0.0.2 17981 1002",
    "ipcserver 1 127.0.0.1 17981",
]
```

配了 `Registry.ip` 就连 registry。拉取回包里有名单，还有 `bStatic`。为真只使用名单，为假登记自己的地址并按秒心跳；旧 registry 不带该字段时默认登记。查询失败时缓存保持原状。

下面是 registry 进程。它只起一份，单线程，代码在 `cbaselib/registry`。`Registry.config` 不写或是 `dynamic` 时是动态配置，业务进程要上报。`config = "static"` 时是静态配置，名单在表 `oi_service_instance`，由运维维护，业务进程只拉。现在动态配置用 Redis，静态配置用 MySQL；换引擎不改这两个名字。静态配置的库连接用 `Registry.db_host`、`db_port`、`db_user`、`db_password`、`db_database`，可选 `db_tls_ca`，并且构建要带 MySQL 客户端。

## 注册空间

这一节只在动态配置时用到。所有键都在 `oi:{project.env}:` 下面，`project` 和 `env` 只写在 `registry.toml` 的 `[Registry]`。测试环境和正式环境各起各的 registry，哪怕连同一台 Redis 也互相看不见。没写这两项，或写了字母、数字、`_`、`-` 以外的字符，registry 拒绝启动。花括号是 Redis Cluster 的哈希标签，同一空间的键落在同一个槽里。

| 键 | 内容 |
| --- | --- |
| `svc` | 登记过的名字集合 |
| `svc:<name>` | 这个名字下登记过的 id 集合 |
| `svc:<name>:<id>` | 一份实例，值是 `ip port`，分区服再跟分区号，带 `expire_ms` 的过期 |

## 动态配置现在连 Redis

用 `RedisClient`，直接连真正的 Redis，配置在 `registry.toml` 的 `[RedisClient]`，不写 `service`。每条登记、续期、撤销和整量拉取都是 `co_await CallRedis` 一段 Lua，等 Redis 的时候 poll 照转，别的连接照常收发。`timeout_ms` 要比 `expire_ms` 短，Redis 卡住时服务还来得及重新登记。

Redis 连不上或卡住时，登记和续期回失败，客户端下一次心跳重来；整量拉取失败就回失败，客户端保持原状，不把对端判成下线。 启动时 `Publish` 若未收到登记确认，会记录失败原因、接受后台心跳重试并返回 `OI_OK`；这表示已接受重试，不表示同步登记已成功。收到实例号已在线的明确回执时返回 `OI_ERR_EXIST`。

## 登记和在线

`id` 取 `[Service] id`，必须是正整数。监听方 `StartServer` 成功后同步发一次登记，最多等 1 秒。登记的是监听段上的 `advertise_ip` 和 `advertise_port`，没有再看 `Service` 下的同名项，都没有才用监听地址。绑在 `0.0.0.0`、`::`、`[::]` 或 `*`，又没配对外地址时不登记。登记值是 `ip port`，IPv6 的冒号留在地址里。`::1` 和 `[::1]` 文本不同，算两个地址。

名字、id、地址都一样就是同一个服务，新连接重新登记后接管租约；被接管的旧连接对该实例再发心跳或登记会收到 `OI_ERR_EXIST`，旧连接关闭不会撤销新连接的登记。新连接关闭后，后续全新连接仍可重新登记。registry 重启后服务连回来也是这样。同一空间里同名同 id、地址不一样的还在线，回 `OI_ERR_EXIST`，后起的进程拒绝启动。

之后每隔 `heartbeat_ms` 报一次在线，registry 按这条连接上的登记续期。键因为停顿过期、又没人占，就重新占上。已经被别的进程占了，就停止登记、报错，不去抢。

登记的那条连接一断，registry 当场撤掉它登记的键，调用方下一次拉取就看到下线。冻住的进程连接还在，靠过期判下线，最慢 `expire_ms + pull_ms`。上下线、被占都打日志。

## 调用方

配置里写了 `<段名>.service` 时，`SessionManager::Init` 先 `Lookup` 同步拉一次这个名字，查不到再用同一段的 ip/port。之后 `RegistryClient` 的单例秒更新 `OnSecondUpdate` 按 `pull_ms` 整量拉取关心的名字，运行期选路只读本地缓存。动态配置下，registry 每隔 `pull_ms` 从当前引擎整量取一次，过期的 id 和空了的名字顺手摘掉，客户端来拉时直接给这份。静态配置按同样的间隔重读名单表。一个名字下有多份实例时选 id 最小的，连上之后固定走它，它下线了才换。

连接是不是同一条，用 `NetAddr::Endpoint` 做键。IPv4 是 `ip:port`，IPv6 是 `[ip]:port`。换到新地址时只拆掉连着旧地址的会话，其他服务的会话不动。地址没变就不动。

## 现有会话管理器

`SessionManager::Init` 用 `GetIDString()` 当配置段名，读 `<名字>.ip`、`<名字>.port`、`<名字>.type`，查到注册就用查到的地址。

| 类 | 字节流 | 为什么单独存在 |
| --- | --- | --- |
| `IpcClient` / `IpcServer` | RPC 帧，用基类 `ProtoSession` | 几乎只有 `GetIDString`，以及连上之后起测试 |
| `LogClient` / `LogServer` | 标准 RPC 单向消息帧 | 日志服保留专用会话解析，以支持写入背压 |
| `RedisClient` | RESP | 同样换了会话类型 |
| `RedisServer` | RESP，直接继承 `SessionManager` | 还要按连接把命令转给上游 Redis |

`webclient` / `webserver` 走 HTTP，不在这张表里。

## 后续：同一种字节流收成一套

`IpcClient` / `IpcServer` 可以不再各写一个子类。按服务名起一个 `ProtoClientManager` / `ProtoSessionManager`，连上、断开时要跑的测试用回调挂上。

日志使用标准 RPC 单向消息帧，日志服保留专用解析和背压；Redis 使用 RESP。管理器按服务名来，会话类型按编码和背压需求来。

## sessionid

不能从框架里删。一个监听进程上同时有多条连接，回包、断连、Redis 把命令转回哪个客户端，都靠 `SessionManager::SendBytes(sid)` 这张表区分。

客户端本来就是一条连接一个 `iSessionId`。业务里的 `RpcCall(mgr, sid, ...)` 可以收成按服务名调用，sid 留在管理器内部。服务端处理函数如果只填回包，签名里也可以不再出现 sid；`RpcHandlerBuilder::Run` 回包时仍拿着这次的 sid。`redisserver` 的 `Issue` / `OnReply` 把 sid 当成「这条客户端连接」用，这块拿不掉。

同一服务名有多份实例时，靠下面的全局服和分区服区分，不是把 sid 删掉。

## 全局服和分区服

服务名里不能有 `.` 和 `*`。分区号单独传入，不拼进服务名。

按服务名的无状态调用走进程里唯一的 `OI::Rpc::RouteManager::GetInstance()`。单次把回复对象传进去，`co_await` 得到 `int32_t`。

| 调用 | 含义 |
| --- | --- |
| `Call(名字, req, res)` | 任选一台，优先已连上的。 |
| `Call(名字, 分区号, req, res)` | 打这个分区里实例号最小的一台。全局服不接受分区号。分区号为负返回 `OI_ERR_FAIL`，不发请求。 |
| `CallAll<Res>(名字, req)` | 协程挂起读本地缓存，没拉到过就等下一次秒更新，再同时发给每一份。等待不占线程，上限 min(剩余预算, 3000ms)。结果按实例号升序；选路失败时只有一格。 |
| `CallMany<Res>(名字, 分区号列表, req)` | 只打 `vector<int32_t>` 里的这些分区，结果数量、顺序始终和列表一致，重复分区也保留。没登记的那一格是 `OI_ERR_CONNECT`。选路超时等整体错误填入每一格；负数分区号、全局服带分区号每格为 `OI_ERR_FAIL`。空列表返回空结果。 |

路由上的单向消息和上面四个调用对应，默认超时 180 秒：`Notify(名字, msg)` 任选一台，`Notify(名字, 分区号, msg)` 指定分区，`NotifyAll(名字, msg)` 发给这一刻名单里的每一台，`NotifyMany(名字, 分区号列表, msg)` 只发这些分区。返回 `OI_OK` 只表示任务已启动。分区号为负同步返回 `OI_ERR_FAIL`，不启动任务。选路、发送或回执失败由协程任务错误处理器报告（`OI::Coro::ReportTaskError` / `OI::Coro::SetTaskErrorHandler`）。不存在的服务或分区走这条路径，路由失败映射为 `OI_ERR_CONNECT`。这些连接是 `PeerConnection`，断线后还没发出的通知先暂存，重连成功再补发。

登记时标明自己是全局还是分区。全局服拒绝带分区号的调用，避免把一封信送到另一类进程上。

`Call` 一次只打一台。`CallAll` 和 `CallMany` 先把要打的请求全部发出去，再等各自的回包。总耗时取决于最慢的一台，不是各台延迟相加。时长从调用一开始算，选路花掉的时间从同一份 `RpcDeadline` 里扣，发出去只带剩余毫秒；预算用完的格子不再发送，记 `OI_ERR_TIMEOUT`。等待表不用改：每个仍是一个调用号、一份回包、唤醒一次。没回来的那个自己超时或断连，不连累另外几个。等待期间新登记的分区不补进这一批，`CallMany` 也不补列表里没写的分区。

分区号只作为登记字段和 `Call` 的参数，不编进 `sessionid`，也不编进 IP。同一分区再开一份进程时，仍是多条连接，`sessionid` 负责把回包送回其中一条。

## 每台各连一条

`ProtoClientManager` 同一个名字只连一台。要同时直连同名的多台全局服，继承 `ProtoMultiClientManager`，配置段和普通客户端一样：

| 配置 | 名单 |
| --- | --- |
| 写了 `service`，注册表查得到 | 名单里每个 id 一条常驻连接，实例号就是登记的 `[Service] id` |
| 写了 `<段名>.instances` | `["实例号 ip 端口 [分区]", ...]`。配了就不要求 ip/port。没配注册或注册里还查不到时先连这份名单 |
| 没写 `service`，也没写 `instances` | 按段里的 ip/port 连一台，实例号 0 |

`Init` 之后名单已经有了，`Start` 对每台发起建连；不用 `StartClient`。连上回调 `OnPeerConnect(实例号, sid)`，断开回调 `OnPeerDisconnect(实例号, sid)`，都在 poll 线程。断开后自己重连，连不上隔 `RetryMilliseconds()` 再试。

注册表按 `pull_ms` 下发这个名字的整份名单（`RegistryClient::WatchList`），名单变化时调用 `ApplyServiceList`；新订阅者在下一次成功拉取时也会收到名单，即使共享名单没有变化。新登记的 id 自动建连；消失的 id 退役，在途调用回 `OI_ERR_CONNECT`；同一个 id 换了地址先退役再新建。

| 调用 | 含义 |
| --- | --- |
| `Call(实例号, req, res)` | 只打这一台。名单里没有回 `OI_ERR_CONNECT`；在名单里还没连上就暂存，连上补发。 |
| `CallAny(req, res)` | 在已连上的实例里轮询，一台都没连上时在名单里轮询。 |
| `CallMany<Res>(实例号列表, req)` | 只打这些实例，顺序和列表一致。名单里没有的那一格是 `OI_ERR_CONNECT`。 |
| `CallAll<Res>(req)` | 按发送这一刻的名单每台一份，先全部发出再等。结果按实例号升序，每格带 `iInstanceId`。 |
| `NotifyPeer(实例号, msg)` | 向指定实例启动异步通知，名单里没有返回 `OI_ERR_CONNECT`，未连接时暂存并重连。 |
| `NotifyAny(msg)` | 选路同 `CallAny`，名单为空返回 `OI_ERR_CONNECT`。 |
| `NotifyAll(msg)` | 按这一刻的名单给每台各启动一条，返回启动了几条。线程不对返回错误码。未连接时暂存并重连。 |
| `NotifyMany(实例号列表, msg)` | 只发这些实例，名单里没有的跳过，返回启动了几条。线程不对返回错误码。未连接时暂存并重连。 |

通知复制消息并启动任务，通过现有 RPC 路径发送，默认超时 180 秒。`NotifyPeer`、`NotifyAny` 返回 `OI_OK`，`NotifyAll`、`NotifyMany` 返回启动了几条；这些返回值只表示任务已启动，不代表对端收到。发送、回执或超时失败由 `OI::Coro::ReportTaskError` 交给 `OI::Coro::SetTaskErrorHandler`。

服务端推下来的消息登记在接收它的管理器上：自己的 `Init` 里写 `RegisterNotifyHandler(&类::成员)`，别处写 `管理器.RegisterNotifyHandler(&类::成员)`。成员函数里的 `this` 就是这个管理器。`InstanceIds()` 给当前名单，`ConnectedSid(实例号)` 给这台当前的会话，`IsConnect()` 表示至少有一台连着。

写死地址时运行中要换服务器，在 `Init` 之后调 `UseAddress(ip, port)`：旧连接退役，在途调用回 `OI_ERR_CONNECT`，实例号仍是 0，`Start` 过就马上连新地址，`GetStrIP()`、`GetStrPort()` 跟着变。配了 `service` 时返回 `OI_ERR_INVALID_ARGUMENT`；成功返回 `OI_OK`，地址没变什么都不做。

模块生成器生成的 Client 组件默认就是这个类，Server 和 Client 都是单例，带 `OI::Coro::Task OnSecondUpdate(time_t)`，默认函数体是 `co_return;`。回归见 testing.md 的 multi。

## 应用侧看不到 Session

`Session` 和 `SessionManager` 留在框架里。应用不继承它们，也不调用 `SendBytes`、`CloseSession`、`CloneSession`。

应用只做三件事：声明自己是哪个服务（名字、全局还是分区、编码），登记处理函数，按服务名调用。连接、重连、未连上时的暂存、按名字选实例，都在框架里。

回声已经是这种处理函数，注册在 `ipcserver` 的 `Init`：

```cpp
static int32_t HandleAllEcho(const OI::RpcData::AllEchoReq& stRequest, OI::RpcData::AllEchoRes& stResponse)
{
    stResponse.stData = stRequest.stData;
    stResponse.iCode = 1;
    return 0;
}
```

发出去不传管理器和 `sessionid`。时长是 `OI::Timer::MS`，不写就用 `Seconds(180)`：

```cpp
auto& stRoute = OI::Rpc::RouteManager::GetInstance();
OI::RpcData::AllEchoRes stAny;
const int32_t iAny = co_await stRoute.Call("gameserver", req, stAny);
OI::RpcData::AllEchoRes stOne;
const int32_t iOne = co_await stRoute.Call("gameserver", 1001, req, stOne);
auto vSome = co_await stRoute.CallMany<OI::RpcData::AllEchoRes>("gameserver", std::vector<int32_t>{1001, 1002}, req, OI::Timer::Seconds(3));
auto vAll = co_await stRoute.CallAll<OI::RpcData::AllEchoRes>("gameserver", req);
```

服务名和分区号分开传，不拼 `名字.分区`，也不再解析 `名字.*`。`CallAll` 和 `CallMany` 自己先把这一批请求都发出去，再逐个等已经在途的等待器，然后把每台的结果一起交回来。调用方不写这个循环。总耗时取决于最慢的一台。某一台超时或断连只记在它自己的结果里。

单次 `RpcCall` 仍把发送写在 `await_suspend` 里。一批调用不能先造一串还没发送的等待器再挨个 `co_await`，那样会等完一台才发下一台。`CallAll` 和 `CallMany` 在里面先 `Submit` 再等。构建或提交中途抛异常时，只等待已成功登记的调用，再向调用方抛出原异常；未登记项没有等待者可唤醒。

日志、Redis、访问本框架里的 HTTP 服务，也是这种调用，编码留在框架里。应用不写会话子类。对外的 HTTP 网址，以及 `redisserver` 背后那台真正的 Redis，不是注册表里的服务，地址仍要配置。

监听方的一条连接在框架内部仍是一个 `sessionid`。普通处理函数看不到它。需要顺着这条连接回呼时，处理函数第一个形参写 `const OI::Rpc::RpcContext& stContext`，用 `stContext.iSessionId` 和 `stContext.stManager`，只在这次处理期间有效。断连按管理器指针和 `sessionid` 一起摘等待，另一个管理器上的相同数字不受影响。

批量调用把服务名拷进任务，任务跑起来再读本地缓存，没拉到过就等下一次秒更新，等待上限是 min(剩余预算, 3000ms)。这一等是挂起，不占线程。创建任务后可以修改或销毁原来的字符串。目标名单以当时的缓存为准，这次之后新登记的不补进这一批。路由连接池管理器常驻，不为每次调用新增共享所有权；连接失效仍由发送、断连和超时路径返回错误。

按登记名单选定目标后，单次和批量调用都走 `RouteManager` 上这个服务名的常驻连接，不借用调用方自己的连接。选路之后这次请求的目标不变；原目标连接失败时返回错误，不自动改投其他分区。

## 运行期缓存、超时与路由连接回收

起服用 `Publish`、`PullSync`、`Lookup` 同步登记并强制拉一次，结果写入本地缓存。这三步要真正等待时，不能在 poll 回调（含定时器回调）里调用，会抛 `logic_error`。

运行期不再按调用向 registry 发查询。`RegistryClient` 继承 `ProtoSessionManager`，声明在 `cbaselib/proto/inc/oi_registry_client.h`；`cbaselib/base/inc/oi_registry.h` 只保留 `ServiceInstance`、`ServiceTable`、`StaticRegistry`。它是单例，日历秒变化时启动 `OI::Coro::Task OnSecondUpdate(time_t)`，上一次没 `co_return` 就跳过这一秒：没连就连；到点登记或心跳；到 `Registry.pull_ms` 或 `Snapshot` 未命中催促时，把关注的名字整量拉一次。登记、心跳、拉取在这条协程里按顺序 `co_await OI::Rpc::RpcCall(*this, sid, 请求, 回复, 1000ms)`。拉取失败则下一秒重拉。`Snapshot` 只读这份缓存，还没拉到过返回 false，并记下这个名字；未命中不立刻拉取，由下一次秒更新拉。

路由发协议前读本地缓存。命中立即解析。没拉到过就每 10ms 复查，直到拉到或超过 min(剩余预算, 3000ms)，超时返回 `OI_ERR_TIMEOUT`。名单里没有目标时按不在线返回 `OI_ERR_CONNECT`。

注册、心跳和拉取均使用标准 RPC：请求与回复共用协议号，请求体为带请求标记的调用号和请求数据；回复体为调用号、状态码以及成功时的响应数据。日志使用调用号为零的标准单向消息帧。两端必须同步升级，不能与旧帧格式混用。

命名 RPC 的时长覆盖读缓存、建连排队和回包。缓存未就绪时最多再等 min(剩余预算, 3000ms)，预算耗尽不再发送。调度和时间轮粒度仍可能使返回时间略晚于截止时间，不能按硬实时保证使用。

路由连接的发送队列、接收缓冲、帧上限和半包时限取配置段 `RpcRoute`，也就是 `RouteManager` 自己的会话配置。进程里只有这一个路由管理器，所有服务名共用它的 SID 空间；每个服务名一组连接，名单跟着注册表走。实例离开名单才退役。没有空闲回收，也没有客户端数量上限。连接断开后按原地址重连，连上前的调用在这条连接上排队。已发送的请求不会自动重发。
