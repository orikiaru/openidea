# 服务注册

注册只走 `registry` 进程。应用不碰 Redis，只连 registry：启动时登记，定时报在线，定时整量拉取自己关心的名字。registry 只起一份，单线程，代码在 `modules/registry`。

## 注册空间

所有键都在 `oi:{project.env}:` 下面，`project` 和 `env` 只写在 `registry.toml` 的 `[Registry]`。测试环境和正式环境各起各的 registry，哪怕连同一台 Redis 也互相看不见。没写这两项，或写了字母、数字、`_`、`-` 以外的字符，registry 拒绝启动。花括号是 Redis Cluster 的哈希标签，同一空间的键落在同一个槽里。

| 键 | 内容 |
| --- | --- |
| `svc` | 登记过的名字集合 |
| `svc:<name>` | 这个名字下登记过的 id 集合 |
| `svc:<name>:<id>` | 一份实例，值是 `ip port`，带 `expire_ms` 的过期 |

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

客户端本来就是一条连接一个 `_ssid`。业务里的 `RpcCall(mgr, sid, ...)` 可以收成按服务名调用，sid 留在管理器内部。服务端处理函数如果只填回包，签名里也可以不再出现 sid；`RunHandledRpc` 发回包时自己还拿着它。`redisserver` 的 `Issue` / `OnReply` 把 sid 当成「这条客户端连接」用，这块拿不掉。

同一服务名有多份实例时，靠下面的全局服和分区服区分，不是把 sid 删掉。

## 全局服和分区服

服务名里不能有 `.` 和 `*`。对端写成下面三种，只认结尾，不在中间搜索。

| 写法 | 含义 |
| --- | --- |
| `logd` | 全局服。登记时不带分区。多份实例时选一台，连上之后固定走这一条，断了再选。 |
| `gameserver.1001` | 分区服的指定分区。点号后面整段是分区号，登记时带上这个分区。 |
| `gameserver.*` | 按发送这一刻的登记名单，同时发给每一份。 |

登记时标明自己是全局还是分区。全局服拒绝带分区号的写法，分区服拒绝不带分区号的写法，避免把一封信送到另一类进程上。

`name` 和 `name.分区` 一次只打一台。`name.*` 先把这一刻名单上的请求全部发出去，再等各自的回包。总耗时取决于最慢的那一台，不是各台延迟相加。等待表不用改：每个仍是一个调用号、一份回包、唤醒一次。没回来的那个自己超时或断连，不连累另外几个。等待期间新登记的分区不补进这一批。

分区号只放在服务名后缀里，不编进 `sessionid`，也不编进 IP。同一分区再开一份进程时，仍是多条连接，`sessionid` 负责把回包送回其中一条。

## 应用侧看不到 Session

`Session` 和 `SessionManager` 留在框架里。应用不继承它们，也不调用 `SendBytes`、`CloseSession`、`CloneSession`。

应用只做三件事：声明自己是哪个服务（名字、全局还是分区、编码），登记处理函数，按服务名调用。连接、重连、未连上时的暂存、按名字选实例，都在框架里。

现在回声的处理函数还带着 `SessionManager*` 和 `sid_t`，函数体里却不用。收成下面这样：

```cpp
Handle<AllEchoReq, AllEchoRes>([](AllEchoReq& req, AllEchoRes& res) -> coro::Task {
    res.data = req.data;
    res.code = 1;
    co_return;
});
```

发出去也不再传管理器和 `sessionid`：

```cpp
auto res = co_await RpcCall<AllEchoRes>("ipcserver", req);
auto one = co_await RpcCall<AllEchoRes>("gameserver.1001", req);
auto batch = co_await RpcCallBatch<AllEchoRes>("gameserver.*", req);
```

`RpcCallBatch` 自己先把这一批请求都发出去，再逐个等已经在途的等待器，然后把每台的结果一起交回来。调用方不写这个循环。总耗时取决于最慢的一台。某一台超时或断连只记在它自己的结果里。

现有的 `RpcCall` 把发送写在 `await_suspend` 里。`RpcCallBatch` 不能先造一串还没发送的等待器再挨个 `co_await`，那样会等完一台才发下一台。发送和等待拆开，拆在函数里面。

日志、Redis、访问本框架里的 HTTP 服务，也是这种调用，编码留在框架里。应用不写会话子类。对外的 HTTP 网址，以及 `redisserver` 背后那台真正的 Redis，不是注册表里的服务，地址仍要配置。

监听方的一条连接在框架内部仍是一个 `sessionid`。处理函数和调用方都看不到它。
