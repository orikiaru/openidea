# 服务注册

后期要做，现在不改代码。目标是按服务名找到地址再连接，业务配置里不再写对端的 IP 和端口。

## 现在的样子

`SessionManager::Init` 用 `GetIDString()` 当配置段名，固定读 `<名字>.ip`、`<名字>.port`、`<名字>.type`。toml 里的 `[Service] name` 没有参与发现，两端各自写死同一对地址。

现有会话管理器：

| 类 | 字节流 | 为什么单独存在 |
| --- | --- | --- |
| `IpcClient` / `IpcServer` | RPC 帧，用基类 `ProtoSession` | 几乎只有 `GetIDString`，以及连上之后起测试 |
| `LogClient` / `LogServer` | 日志帧 | 换了 `CloneSession`，不走 RPC 解析 |
| `RedisClient` | RESP | 同样换了会话类型 |
| `RedisServer` | RESP，直接继承 `SessionManager` | 还要按连接把命令转给上游 Redis |

`webclient` / `webserver` 走 HTTP，不在这张表里。

## 第一期：注册服务

改动中等，先不拆这些类。

新起一个注册进程。它自己的地址没法靠注册发现，每个进程的配置里仍留这一处，例如 `[Registry] ip/port`。表是「服务名 → 地址」。监听方 `StartServer` 成功后登记，退出或心跳断了就删。调用方配置里写 `service = "ipcserver"` 时先查地址再 `StartClient`；查不到就按现有的 `ip/port` 连，方便还没接注册的进程。

要改的是 `SessionManager::Init` 和各进程的启动路径，以及 6 份 toml 里的对端地址。RPC、日志帧、RESP、`sessionid` 都不动。回声、日志、Redis 的测法只是前面多起一个注册进程。

这一期是新增一个小模块和发现客户端，不是重写网络层。

## 第二期：同一种字节流收成一套

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

// 先全部发出去，再逐个等已经在途的等待器。总耗时取决于最慢的一台。
auto waits = RpcCallBatch<AllEchoRes>("gameserver.*", req);
for (auto& w : waits)
{
    auto item = co_await w;
}
```

`RpcCallBatch` 在返回前把这一批请求都写出去，每个等待器已经挂在自己的调用号上。后面的循环只是收结果。某一台超时或断连只记在它自己的结果里。

现有的 `RpcCall` 把发送写在 `await_suspend` 里，所以不能先造一串还没发送的等待器再挨个 `co_await`，那样会等完一台才发下一台。`name.*` 必须把发送和等待拆开。

日志、Redis、访问本框架里的 HTTP 服务，也是这种调用，编码留在框架里。应用不写会话子类。对外的 HTTP 网址，以及 `redisserver` 背后那台真正的 Redis，不是注册表里的服务，地址仍要配置。

监听方的一条连接在框架内部仍是一个 `sessionid`。处理函数和调用方都看不到它。
