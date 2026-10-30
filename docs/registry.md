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

同一服务名有多份实例时，要另定选哪一个：第一台、随机，或按调用方固定。那是发现策略，不是把 sid 删掉。
