# 服务注册

注册只走 `registry` 进程。应用不碰 Redis，只连 registry：启动时登记，定时报在线，定时整量拉取自己关心的名字。registry 只起一份，单线程，代码在 `modules/registry`。

## 注册空间

所有键都在 `oi:{project.env}:` 下面，`project` 和 `env` 只写在 `registry.toml` 的 `[Registry]`。测试环境和正式环境各起各的 registry，哪怕连同一台 Redis 也互相看不见。没写这两项，或写了字母、数字、`_`、`-` 以外的字符，registry 拒绝启动。花括号是 Redis Cluster 的哈希标签，同一空间的键落在同一个槽里。

| 键 | 内容 |
| --- | --- |
| `svc` | 登记过的名字集合 |
| `svc:<name>` | 这个名字下登记过的 id 集合 |
| `svc:<name>:<id>` | 一份实例，值是 `ip port`，分区服再跟分区号，带 `expire_ms` 的过期 |

## registry 怎么连 Redis

用 `RedisClient`，直接连真正的 Redis，配置在 `registry.toml` 的 `[RedisClient]`，不写 `service`。每条登记、续期、撤销和整量拉取都是 `co_await CallRedis` 一段 Lua，等 Redis 的时候 poll 照转，别的连接照常收发。`timeout_ms` 要比 `expire_ms` 短，Redis 卡住时服务还来得及重新登记。

Redis 连不上或卡住时，登记和续期回失败，客户端下一次心跳重来；整量拉取失败就回失败，客户端保持原状，不把对端判成下线。

## 登记和在线

`id` 取 `[Service] id`，必须是正整数。监听方 `StartServer` 成功后同步发一次登记，最多等 1 秒。登记的是监听段上的 `advertise_ip` 和 `advertise_port`，没有再看 `Service` 下的同名项，都没有才用监听地址。绑在 `0.0.0.0` 又没配对外地址时不登记。

名字、id、地址都一样就是同一个服务，重新登记直接续上，registry 重启后服务连回来也是这样。同一空间里同名同 id、地址不一样的还在线，回 `OI_ERR_EXIST`，后起的进程拒绝启动。

之后每隔 `heartbeat_ms` 报一次在线，registry 按这条连接上的登记续期。键因为停顿过期、又没人占，就重新占上。已经被别的进程占了，就停止登记、报错，不去抢。

登记的那条连接一断，registry 当场撤掉它登记的键，调用方下一次拉取就看到下线。冻住的进程连接还在，靠过期判下线，最慢 `expire_ms + pull_ms`。上下线、被占都打日志。

## 调用方

配置里写了 `<段名>.service` 时，`SessionManager::Init` 先向 registry 查这个名字，查不到再用同一段的 ip/port。之后按 `pull_ms` 拉取关心的名字。registry 每隔 `pull_ms` 用一段 Lua 从 Redis 整量取一次，过期的 id 和空了的名字顺手摘掉，客户端来拉时直接给这份。一个名字下有多份实例时选 id 最小的，连上之后固定走它，它下线了才换。

连接是不是同一条，用 `ip:port` 做键。换到新地址时只拆掉连着旧地址的会话，其他服务的会话不动。地址没变就不动。

## 现有会话管理器

`SessionManager::Init` 用 `GetIDString()` 当配置段名，读 `<名字>.ip`、`<名字>.port`、`<名字>.type`，查到注册就用查到的地址。

| 类 | 字节流 | 为什么单独存在 |
| --- | --- | --- |
| `IpcClient` / `IpcServer` | RPC 帧，用基类 `ProtoSession` | 几乎只有 `GetIDString`，以及连上之后起测试 |
| `LogClient` / `LogServer` | 日志帧 | 换了 `CloneSession`，不走 RPC 解析 |
| `RedisClient` | RESP | 同样换了会话类型 |
| `RedisServer` | RESP，直接继承 `SessionManager` | 还要按连接把命令转给上游 Redis |

`webclient` / `webserver` 走 HTTP，不在这张表里。

## 后续：同一种字节流收成一套

`IpcClient` / `IpcServer` 可以不再各写一个子类。按服务名起一个 `ProtoClientManager` / `ProtoSessionManager`，连上、断开时要跑的测试用回调挂上。

日志和 Redis 的 `OnDataIn` 不是 RPC 帧。并进 `ProtoSession` 等于改协议。标准结构是：管理器按服务名来，会话类型按编码来。管理器的样板能减掉，日志会话和 Redis 会话还在。

## sessionid

不能从框架里删。一个监听进程上同时有多条连接，回包、断连、Redis 把命令转回哪个客户端，都靠 `SessionManager::SendBytes(sid)` 这张表区分。

客户端本来就是一条连接一个 `iSessionId`。业务里的 `RpcCall(mgr, sid, ...)` 可以收成按服务名调用，sid 留在管理器内部。服务端处理函数如果只填回包，签名里也可以不再出现 sid；`RunHandledRpc` 发回包时自己还拿着它。`redisserver` 的 `Issue` / `OnReply` 把 sid 当成「这条客户端连接」用，这块拿不掉。

同一服务名有多份实例时，靠下面的全局服和分区服区分，不是把 sid 删掉。

## 全局服和分区服

服务名里不能有 `.` 和 `*`。分区号单独传入，不拼进服务名。

| 调用 | 含义 |
| --- | --- |
| `CallAny` | 不带分区号。在当前登记里随机一台。管理器自己已经连着这个名字时，继续用这条连接，断了再选。 |
| `Call(分区号)` | 只打这个分区。全局服不接受分区号。 |
| `CallAll` | 协程挂起拉取当前登记名单，再同时发给每一份。等待不占线程。 |
| `CallMany(分区号列表)` | 只打 `vector<int32_t>` 里的这些分区，结果数量、顺序始终和列表一致，重复分区也保留。没登记的那一格是 `OI_ERR_CONNECT`。查询超时、限流等整体错误填入每一格；负数分区号、全局服带分区号每格为 `OI_ERR_FAIL`。空列表返回空结果。 |

登记时标明自己是全局还是分区。全局服拒绝带分区号的调用，避免把一封信送到另一类进程上。

`CallAny` 和 `Call` 一次只打一台。`CallAll` 和 `CallMany` 先把要打的请求全部发出去，再等各自的回包。总耗时取决于最慢的一台，不是各台延迟相加。时长从调用一开始算，选路花掉的时间从同一份 `RpcDeadline` 里扣，发出去只带剩余毫秒；预算用完的格子不再发送，记 `OI_ERR_TIMEOUT`。等待表不用改：每个仍是一个调用号、一份回包、唤醒一次。没回来的那个自己超时或断连，不连累另外几个。等待期间新登记的分区不补进这一批，`CallMany` 也不补列表里没写的分区。

分区号只作为登记字段和 `Call` 的参数，不编进 `sessionid`，也不编进 IP。同一分区再开一份进程时，仍是多条连接，`sessionid` 负责把回包送回其中一条。

## 应用侧看不到 Session

`Session` 和 `SessionManager` 留在框架里。应用不继承它们，也不调用 `SendBytes`、`CloseSession`、`CloneSession`。

应用只做三件事：声明自己是哪个服务（名字、全局还是分区、编码），登记处理函数，按服务名调用。连接、重连、未连上时的暂存、按名字选实例，都在框架里。

回声已经是这种处理函数，注册在 `ipcserver` 的 `Init`：

```cpp
static OI_RPC_SYNC_HANDLER(AllEcho, HandleAllEcho)
{
    stResponse.stData = stRequest.stData;
    stResponse.iCode = 1;
    return 0;
}
```

发出去不传管理器和 `sessionid`。时长是 `OI::Timer::MS`，不写就用 `Seconds(180)`：

```cpp
auto any = co_await games.CallAny<AllEchoRes>(req);
auto one = co_await games.Call<AllEchoRes>(1001, req);
auto some = co_await games.CallMany<AllEchoRes>(std::vector<int32_t>{1001, 1002}, req, OI::Timer::Seconds(3));
auto all = co_await games.CallAll<AllEchoRes>(req);
```

服务名在管理器上，调用方不拼 `名字.分区`。`CallAll` 和 `CallMany` 自己先把这一批请求都发出去，再逐个等已经在途的等待器，然后把每台的结果一起交回来。调用方不写这个循环。总耗时取决于最慢的一台。某一台超时或断连只记在它自己的结果里。

单次 `RpcCall` 仍把发送写在 `await_suspend` 里。一批调用不能先造一串还没发送的等待器再挨个 `co_await`，那样会等完一台才发下一台。`CallAll` 和 `CallMany` 在里面先 `Submit` 再等。

日志、Redis、访问本框架里的 HTTP 服务，也是这种调用，编码留在框架里。应用不写会话子类。对外的 HTTP 网址，以及 `redisserver` 背后那台真正的 Redis，不是注册表里的服务，地址仍要配置。

监听方的一条连接在框架内部仍是一个 `sessionid`。普通处理函数看不到它。需要顺着这条连接回呼时，用 `OI_RPC_CONTEXT_HANDLER` 里的 `stContext.iSessionId` 和 `stContext.stManager`，只在这次处理期间有效。断连按管理器指针和 `sessionid` 一起摘等待，另一个管理器上的相同数字不受影响。

批量调用把服务名拷进任务，任务跑起来再向注册表要名单，这一等是挂起，不占线程。创建任务后可以修改或销毁原来的字符串。目标名单以这次回包为准，回包之后新登记的不补进这一批。路由连接池管理器常驻，不为每次调用新增共享所有权；连接失效仍由发送、断连和超时路径返回错误。

按登记名单选定目标后，单次和批量调用都使用固定地址的池内连接，不复用可能随订阅更新而改连其他分区的调用方管理器。选路后即使订阅管理器换区，这次请求的目标也不变；原目标连接失败时返回错误，不自动改投其他分区。`CallAny` 已有连接的快捷路径仍保留，因为它不限定分区。

## 查询容量、超时与路由连接回收

`Registry.max_pending_pulls` 默认 4096，限制未发送与已发送查询的合计数量；超限返回 `OI_ERR_BUSY`。查询使用单调时钟截止时间，周期扫描随时间轮 tick 推进。未发送查询到期直接摘除；已发送查询到期时失效并关闭该代连接、清空配对队列，避免迟到回包错配下一条查询。到期项返回 `OI_ERR_TIMEOUT`，同连接被连带终止的其他查询返回 `OI_ERR_CONNECT`；后续调用可重新建连。线上注册协议格式保持不变。

命名 RPC 的时长覆盖查询、建连排队和回包，查询完成后仅使用剩余预算，预算耗尽不再发送。注册查询另保留最多 1 秒的故障检测；调度和时间轮粒度仍可能使返回时间略晚于截止时间，不能按硬实时保证使用。

成员调用选出的路由连接继承该管理器的发送队列、接收缓冲、帧上限、半包时限及连接前请求排队额度。不同容量策略不共用连接；不传管理器的自由函数使用框架默认策略。

`RpcRoute.max_clients` 默认 1024，`RpcRoute.idle_ms` 默认 60000，均须为正数。池满返回 `OI_ERR_BUSY`。路由管理器是进程常驻单例，所有目标共用管理器和 SID 空间，动态对象只是连接状态与会话；不同策略的限制保存在各自连接/会话中，不修改共享管理器配置。

出站请求和入站处理在执行期间持有连接业务租约，完成后释放；闲置扫描只回收无业务租约的连接，并取消未完成建连。会话和建连通道只持有连接状态，不持有管理器共享指针。连接断开后，下一次调用按需建连并在当前连接状态中有界排队，不依赖“曾经连接成功”的标记；已发送的请求不会自动重发。回收检查有周期粒度，不保证在 `idle_ms` 到点的同一毫秒释放。
