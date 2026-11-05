# 基础库

`cbaselib` 编成两套静态库。多线程是 `base_m_static`、`net_m_static`、`proto_m_static`、`timer_m_static`、`logger_m_static`、`redis_m_static`、`http_m_static`。单线程是不带 `_m_` 的同名库，编译时定义 `_SINGLE_THREAD_`。

`_SINGLE_THREAD_` 下 `Mutex` 和 `SpinLock` 的加锁是空操作。多线程的 `Mutex` 是 `std::recursive_mutex`。

## 目录

`base` 是进程里的公共件。`Bytes` 是二进制缓冲，`BytesStream` 做序列化。`Config` 读 toml。`ThreadPool` 跑普通任务和协程句柄。应用侧用 `AddTask(lambda)` 或协程，不要自己继承 `Task` 再 `new`/`delete`。`Task` 的分配留在线程池里。`DECLARE_SINGLETON(T)` 提供 `T::GetInstance()`，返回唯一实例的引用，不进入继承链。`FilePath` 检查路径在不在，并补齐中间目录。

`net` 是网络，类型在 `OI::Net`。`Poll` 包 libuv 的事件循环。`NetSocket`、`AcceptChannel`、`ConnectChannel`、`TcpChannel` 负责监听、连接和收发。`Session` 和 `SessionManager` 管一类连接，组件配置里的 ip、port、type 从这里读。`SendBytes` 只在查会话表时持管理器锁，拷贝数据在锁外、持会话锁做；会话带引用计数，通道销毁时先断开会话到通道的指针再释放引用。监听口在 `bind` 之前打开 `SO_REUSEADDR`。进程退出后旧连接还会在 `TIME_WAIT` 里待一会儿，但不再挡住下一次监听。客户端连出去不走 `bind`。按服务名发现地址在 [服务注册](registry.md)。表由 `registry` 进程管，存在 Redis，应用只连 registry。配置里写了 `<名字>.service` 时，`SessionManager::Init` 先向 registry 查，查不到再用同一段的 ip/port。监听方在 `StartServer` 成功后登记并定时报告在线。空间按 registry 的 `[Registry] project/env` 分开。

`proto` 是自有协议，类型在 `OI::Rpc`。`Protocol` 是协议基类，`Rpc` 是远程调用。`oi_rpc_coro.h` 是协程任务和 RPC 等待。收包按协议号交给 `DeliverRpc`，不再按协议号克隆存根。`parser` 做组包。`proto_tool` 是 cmake 时跑的 Python 生成器。

`timer` 是分层时间轮，类型在 `OI::Timer`，见 [定时器](timer.md)。应用层时长是 `MS`，由 `Milliseconds`、`Seconds`、`Minutes`、`Hours`、`Days` 构造。

`logger` 是日志客户端，类型在 `OI::Log`。业务进程用它把日志发到 `logd`。`logd` 自己的服务端在 `modules/logd`。`SendLog` 在日志服还没连上时先攒着，待发超过 4MB 就返回 false，调用方自己决定要不要再试。

logd 的 `[Log] max_pending_bytes` 默认 64MiB，所有日志级别共享，包含排队和正在写入、尚未 fflush 的批次及时间前缀。满时保留完整输入帧并暂停该连接读取，写线程完成后通过 poll 恢复；连接关闭后仍排空已读取的完整帧。进程正常退出先排空等待会话，再 join 写线程。本地日志返回 false 表示当前不能接纳，仍已按原规则输出到 stdout；不能在 poll 中阻塞等待磁盘。此限额不含接收缓冲、内核缓冲及分配器开销，不是 RSS 上限。

`python3 tools/run_regressions.py --case log-backpressure` 使用现有 LogMgr 发送 64MiB、通过临时 LD_PRELOAD 只延迟 logd 写盘线程，核对每条落盘正文、实际字节、队列峰值不超限、发生暂停且最终 pending=0；普通日志用例继续验证 1GiB 吞吐。全套 `--case all` 包含慢盘用例，两种客户端/服务端线程模式均需运行。

`redis` 是连 `redisserver` 的客户端，类型在 `OI::Redis`，字节是 RESP，不进 proto。一条连接上的命令按发送顺序对回包。超时后这条命令仍留在队列里，等它自己的回包来认领，避免后面的应答串位。回包如果再也没来，而连接还在，后面的命令会一直顶在它后面。超时单位是毫秒，来自 `RedisClient.timeout_ms`，由 200 毫秒的周期定时器扫描。先到的那个算数。`RedisResult.iStatus` 区分 Ok、Timeout、Disconnected、InvalidArgument、SendFailed、ServerError、ProtocolError、Busy；`bTimeout` 仅在真正超时时为真。错误回执仍保留在共享 `pReply` 中，`if (result)` 只对 Ok 成立。

标准命令包含 `oi_redis_cmd.h`；只用原生客户端则包含 `oi_redis.h`，两个入口不循环包含。常用命令不再返回 hiredis 回执：`int32_t iRet = co_await RedisGet("key", stOutput)`，`co_await RedisSet("key", stValue)`。输出为 `std::string` 时直接接收二进制字符串，为 rpcdata 时按模板类型反序列化。写入 rpcdata 使用其现有二进制序列化，不增加额外包头；读写双方须约定同一数据类型，不能靠字节自动推断协议类型。key 和 hash field 均为 `std::string`，支持内嵌 NUL。返回非 0 状态码时不覆盖原输出，数组/字典整份解码成功才提交。内存分配失败或自定义 rpcdata 抛出的其他 C++ 异常仍可传播；自定义输出类型的最终移动赋值应保证不抛异常，不承诺对任意抛异常赋值做事务回滚。

错误码统一为 `int32_t`：0 成功；连接、超时、容量不足沿用 `OI_ERR_CONNECT / TIMEOUT / BUSY`；不存在的标量返回 `OI_ERR_NOT_FOUND`，空字符串和空集合仍然成功；错误 key 类型为 `OI_ERR_REDIS_WRONGTYPE`；其他服务端错误为 `OI_ERR_REDIS`；回执形状错误为 `OI_ERR_PROTOCOL`；rpcdata 内容损坏为 `OI_ERR_DECODE`，序列化失败为 `OI_ERR_ENCODE`。Redis 返回的计数、排名或分数通过输出参数取得，不把它们当错误码。读取 TTL 的 -1/-2 是成功返回的数据，含义沿用 Redis。

命令按五类数据结构提供；统一包含 `oi_redis_cmd.h`，也可按需包含 `oi_redis_string.h`、`oi_redis_hash.h`、`oi_redis_list.h`、`oi_redis_set.h`、`oi_redis_zset.h`。下列名称均加 `Redis` 前缀，同步版本加 `Sync`，返回统一状态码，原生数据通过输出参数取得。

- key/string：Get、Set（可带毫秒 TTL）、SetNx、GetSet、GetDel、GetEx、GetPersist、Append、StrLen、GetRange、SetRange、Incr、Decr、IncrBy、DecrBy、IncrByFloat；Del、Unlink、Exists、Type、Ttl、PTtl、Expire、ExpireAt、PExpireAt、Persist。
- hash：HSet（单字段或 map）、HSetNx、HGet、HMGet、HGetAll、HDel（单字段或字段数组）、HExists、HLen、HStrLen、HKeys、HVals、HIncrBy、HIncrByFloat、HScan。
- list：LPush、RPush、LPushX、RPushX（单元素或数组）、LPop、RPop（单元素或指定数量）、LRange、LIndex、LPos、LInsertBefore、LInsertAfter、LRem、LSet、LTrim、LLen。
- set：SAdd、SRem（单成员或成员数组）、SIsMember、SMIsMember、SMembers、SCard、SPop、SRandMember（单成员或指定数量）、SScan。
- zset：ZAdd（单成员或带分数数组）、ZAddNx、ZAddXx、ZAddChanged、ZRem（单成员或成员数组）、ZScore、ZMScore、ZIncrBy、ZRank、ZRevRank、ZCard、ZCount、ZLexCount；ZRange、ZRevRange、ZRangeByScore、ZRevRangeByScore 及其 WithScores 版本；ZRangeByLex、ZRevRangeByLex、ZRemRangeByRank、ZRemRangeByScore、ZRemRangeByLex、ZPopMin、ZPopMax、ZScan。

范围查询保留 Redis 语义：下标可为负；分数边界传字符串（例如 `"-inf"`、`"+inf"`、`"(10"`），字典边界使用 `"-"`、`"+"`、`"[abc"`、`"(abc"`。反向分数/字典查询的参数顺序是最大值、最小值，正向相反。按字典查询要求参与比较的成员分数相同。分数/字典范围可传 offset、count（默认 0、-1，表示全部）；大量数据应分页。字符串局部修改只接收 string，不对 rpcdata 二进制内容做片段修改。

列表和集合的普通结果为 `vector<string>` 或 `vector<rpcdata>`；hash 全量为 `map<string, T>`；带分数结果为 `vector<RedisScored<T>>`，成员为 `stMember`、分数为 `dScore`。SMIsMember 返回与原生整数回执一致的 `vector<int64_t>`（每项 0/1），不用带代理引用的 `vector<bool>`。HMGet 返回 `vector<optional<T>>`，ZMScore 返回 `vector<optional<double>>`，缺失元素是空 optional，保留请求位置，不把整批判成失败。单 key 多元素输入不能为空，空输入返回 `OI_ERR_INVALID_ARGUMENT`。

HScan、SScan、ZScan 返回 `RedisScanPage<T>`：`iCursor` 是下次扫描游标，`stValues` 分别为 map、vector 或带分数 vector。从游标 0 开始，直到返回 0 才结束；中间空页不代表结束，COUNT 是提示而非页大小保证，可能重复返回元素。调用方按业务去重；并发修改时扫描不是快照。MATCH 用原始 Redis glob 匹配，rpcdata 二进制成员通常使用默认 `"*"`。

ZAdd 更新已有成员返回成功，默认计数表示新成员数量，0 不是失败；ZAddChanged 对应 CH，计数包含分数实际变化的成员。SetNx/HSetNx 的 bool 输出表示是否写入。LInsert 未找到 pivot 时返回成功、输出 -1；LPushX/RPushX 对缺失列表输出 0。SPop 返回单元素时空集合为 NOT_FOUND，带数量版本为空数组；LPop/RPop 带数量时缺失 key 仍按 Redis nil 返回 NOT_FOUND。SRandMember 的负 count 允许重复采样。Expire 接收带单位时长，例如 `RedisExpire("key", OI::Timer::Seconds(60), iChanged)`；ExpireAt 是 Unix 秒，PExpireAt 是 Unix 毫秒。

命令语义参照 Redis 官方 [HMGET](https://redis.io/docs/latest/commands/hmget/)、[HSCAN](https://redis.io/docs/latest/commands/hscan/)、[ZRANGEBYSCORE](https://redis.io/docs/latest/commands/zrangebyscore/)、[ZPOPMIN](https://redis.io/docs/latest/commands/zpopmin/)。新增 GetEx/GetDel/LPos/SMIsMember/ZMScore 等接口要求服务端支持对应命令，未知命令保留为服务端错误，不隐式降级或重放。

标准接口严格只操作一个 key；HGetAll、SMembers、范围查询访问的是同一个 key 的多个元素。没有 MGET/MSET、批量 DEL 或隐式跨 key 合并。集群多 key 操作必须由调用方通过原生接口显式指定并处理同槽要求；当前 redisserver 仍是既有代理，不因此获得 Redis Cluster 的 MOVED/ASK 自动路由能力。

PING、Lua 和特殊选项保留原生 `CallRedis({"PING"})` / `CallRedisSync(...)`，不带输出参数时返回共享 hiredis 回执 `RedisResult`，可用 `GetRet()` 取得统一错误码，并按需读取原始回执/错误文本。需要类型化结果时直接 `co_await CallRedis({"PING"}, sPong)`、`co_await CallRedis({"EVAL", "return 42", "0"}, iNumber)`；同步对应 `CallRedisSync(args, output)`。它们同样返回 int32_t，支持字符串、整数、布尔、double、数组、rpcdata 等输出；Lua 返回 rpcdata 的二进制字符串时直接按指定模板解码。Lua 的 false/nil 在 RESP 中是空值，返回 NOT_FOUND，不擅自转换成 bool false；bool 输出只接受整数 0/1。原生命令不会自动合并或改写 key 参数。

依赖前一步结果的命令分条语句等待和检查，不把多个 `co_await` 与同一输出变量的读写塞进一个 `&&` / `||` 表达式。当前 GCC13 回归中，这种复合写法会产生错误的中间结果判断；分步写法保留每个返回码和结果检查，也便于定位哪条命令失败。

应用层直接使用输出参数，不需要自己解析 hiredis 类型：

```cpp
OI::RpcData::RegInstance stInstance;
const int32_t iRet = co_await OI::Redis::RedisGet("service:one", stInstance);
if (iRet != OI_OK)
{
    co_return iRet;
}
// stInstance 已按其 rpcdata 类型反序列化，可以直接使用。

std::vector<OI::Redis::RedisScored<std::string>> vTop;
const int32_t iRangeRet = co_await OI::Redis::RedisZRevRangeWithScores("rank:one", 0, 9, vTop);
if (iRangeRet != OI_OK)
{
    co_return iRangeRet;
}
// 每项 stMember 是成员，dScore 是分数；输出对象都属于当前协程帧。
```

原生 RedisCall 仍只允许 await 一次，重用在发送前抛 logic_error；标准命令返回 `TaskResult<int32_t>`，可共享等待，命令只执行一次。输入在调用返回前已拷贝/序列化；输出对象须活到任务完成。任务副本不代表输出对象所有权，不能在完成前销毁输出，也不能把同一输出用于同时执行的不同请求。

`RedisClient.max_pending` 默认 65536，包含已超时但尚未收到回执的配对项，满后返回 Busy；不会自动重试或重放写操作。超时不等于命令未执行，应用不能无条件重试 INCR、LPUSH 等写操作。

语义依据：[GET](https://redis.io/docs/latest/commands/get/)、[ZADD](https://redis.io/docs/latest/commands/zadd/)、[ZRANGE](https://redis.io/docs/latest/commands/zrange/)。


共享上游连接不支持改变连接状态的命令（切库、认证、事务、订阅、CLIENT 等）。代理收到这类命令会记录错误并关闭该下游连接，避免本地错误回包与在途回包错位。阻塞命令仍会阻塞共用该上游的其他会话，生产环境应单独部署这类调用。多线程版同步 Redis 调用禁止在 poll 线程执行，会抛出 `std::logic_error`；该线程应使用协程接口。

`http` 是 HTTP/1.1 客户端，在 `OI::Http`。套接字是一条 `OI::Net::Channel`，可读走 `DataIn`，可写走 `DataOut`。`co_await` 之后由 poll 线程收完，再 `Schedule` 把协程句柄交回去，不在回调里 `resume`，也不睡线程。查询参数按百分号编码，和 `cpp-httplib` 的 `encode_query_param` 同一套字符。body 按 `Content-Length` 原样发送。单线程模块链 `http_static`，多线程模块链 `http_m_static`。服务端不在这个库里。

`port` 是套接字的平台差异。`port/win` 给 MSVC 补头文件。

`3rd` 放进来一起编的第三方代码：`cpptoml`、`libuv`、`hiredis`、`jsoncpp`。`webserver` 仍用 header-only 的 `cpp-httplib` 听端口。JSON 只用 JsonCpp。Windows 的事件循环用 libuv 自己的实现。

`include` 是给业务包含的汇总头：`oi_header.h`、`oi_base.h`、`oi_net.h`。

## 进程里谁先起来

时序在 [时序图](sequence-diagram/README.md)。生成出来的 `main` 按这个顺序：

1. `OI::Base::StartFramework(配置路径)`：`Config` 读 `deploy/config/<进程名>.toml`，再依次 `ThreadPool::Init`、`BaseTimerManager::Init`、`Poll::Init`。返回非 0 时 `main` 直接把它返回，配置读不了是 -2，框架初始化失败是 -1
2. `OI::Log::Init`
3. 各组件 `Init`，服务端再 `StartServer`，客户端再 `StartClient`
4. `return OI::Base::RunFramework()`：进入主循环，退出后返回退出码

`logd` 不调 `OI::Log::Init`，不作为日志客户端连自己。它在 `LogManager::Init` 之后调 `OI::Log::InitLocal`，自己的日志直接进本地文件。

`[ThreadPool] max_task_count` 限制每个优先级队列里排队的 `Task` 数，满了 `AddTask` 返回 false 并销毁这个任务。不配不限。协程句柄不走这条队列，不受它限制。

## 发送缓冲限额与同步等待

各网络组件段支持 `max_receive_buffer_bytes`，默认 67108872 字节，限制每会话尚未解析的输入。RPC 组件另支持 `max_frame_bytes`（正文上限，默认 67108864）与 `incomplete_frame_timeout_ms`（默认 10000）。接收限额至少为正文上限加 8 字节包头；这些配置必须为正值。包头声明超限立即断连；残包从首次出现开始计时，滴流不延期，完整包取消计时，空闲连接不启动计时。这些限制与 `[Rpc]` 已接收请求的执行额度独立，也不是整个进程的 RSS 上限。

Redis 超时扫描直接运行在 poll 的 200ms 周期计时器上，普通队列线程调用同步接口不会堵住超时。超时项仍保留到对应回执或断连，防止响应串位。`--case redis-queue-timeout` 通过现有 RedisMgr 的 `[Regression] redis_queue_timeout` 开关复测；`--case rpc-ingress` 注入超大声明、残包、滴流和合法边界分片后，再运行现有回声负载。这两项已纳入 `--case all`。

各网络组件配置段可设置 `max_send_queue_bytes`，默认每条连接 67108864（64MiB），配置必须为正数。计数包括会话和通道内尚未交给操作系统的字节，缓冲移动不恢复额度，实际写出才恢复。不是整个进程 RSS 上限，也不包含内核缓冲；大于配置上限的单条消息同样被拒绝，需要按业务大小调整配置。

连接已关闭或容量不足时 `SendBytes` 返回 false，不追加半条消息。客户端 RPC 按发送失败结束等待；日志调用方可让出后重试，框架不自动重放写请求。服务端 RPC、Redis 和注册服务若无法排入回复，会关闭该连接，避免无声丢回复或 RESP 配对错位。会话关闭状态通过原子变量发布，缓冲仍由框架 Mutex 保护，单线程下该锁为空操作。

Redis 同步接口在单线程里也会推进普通任务与超时检查，不再只转网络和协程队列；在 libuv 回调中两种线程模式都明确抛 logic_error，避免重入网络循环。应用优先使用 co_await，单线程同步调用仍可能在等待时运行其他业务。

HTTP 客户端忽略临时 1xx 响应并继续读最终响应（最多 16 个，101 升级不支持）；204/304 在头部完成后结束，不等待 EOF。长度字段必须是完整十进制数；冲突 Content-Length、同时出现 Transfer-Encoding 和 Content-Length、未支持的传输编码、损坏的 chunk 长度及数据分隔符均按协议失败处理。chunked 支持扩展和 trailer。

客户端建连和服务地址切换统一在 poll 线程执行。`StartClient()` 调用即发起非阻塞连接，返回可等待对象；忽略对象为异步启动，`co_await StartClient()` 等结果（成功 true，失败/地址切换/停止入口后 false）。没有同步布尔开关。已有连接时成功，建连中的并发调用共享同一代连接；旧代次迟到的成功或失败不能改变当前状态。等待不占线程，管理器须覆盖网络操作生命周期。

Redis 入队、发送、解析和断连处理都在 poll 线程，等待项与实际发送使用同一个 sid；收包必须携带 sid，旧会话的数据直接丢弃。新连接会先结束旧等待再重建解析器。注册发现的 FIFO 请求入队和发送共用框架锁，发送失败/异常在锁内回滚，旧会话的回复和断连回调不影响新连接。

二进制 bool 字段只接受字节 0 和 1，其他值抛 ParserException，保留输出值及读取位置；普通流和字段格式都做相同校验。HTTP 在完成、失败或超时时先释放请求和原始响应缓冲容量，再唤醒调用方；晚到的超时仅持有轻量状态，不保留大包。

停服时，ThreadPool 先发布 StopRequested，再在 poll 上运行初始化阶段注册的停止回调，最后通知工人退出并 join，之后才停止 poll。Redis 的停止回调以 Disconnected 结束等待并关闭连接；停止后同步/协程 Redis 新调用直接失败，不再排到已退出的网络循环。停止回调不能阻塞；业务自身的 TaskScope 仍须按协程文档在正常退出前 Join。

redisserver 的 `[Redis] max_pending_bytes`（默认 64MiB）和 `max_pending_commands`（默认 65536）按每条上游连接计费，字节数包含完整 RESP 请求；收到回执或上游断连时归还额度，不因客户端超时而提前归还。任一额度不足就保留未发送命令并暂停该下游读取；两项都降到各自限额一半后恢复，并先处理已有输入缓冲。恢复通过 `Poll::PostOnLoop` 排到后续事件循环，不再依赖时间轮；每轮按等待队列轮转，每个会话最多转发一条命令，未处理完的排队尾。额度不足的大请求保留队首，避免被小请求持续挤占；新到请求不能越过已有等待者。单条命令超过字节限额会关闭下游连接，解析长度时即拒绝，不能等待一个永远凑不齐的额度。这是请求积压上限，并非进程 RSS 上限：分配器容量、下游输入缓冲、内核缓冲及响应内存另计。

复测使用 `python3 tools/run_regressions.py --case redis-shutdown`、`redis-backpressure-bytes`、`redis-backpressure-count`；用例都在现有 RedisMgr，通过临时配置的 `[Regression] redis_shutdown / redis_pressure / redis_oversized` 启用。停服测试在同步等待时发送 SIGTERM；背压测试暂停并恢复本机 Redis，检查限额、内存增量及 64 条大响应逐条配对，还验证超大命令拒绝后的重连。`--case all` 包含这三项，需在两种线程模式分别运行。

`Poll::CallOnLoop` 在 poll 线程上可以立即执行；`PostOnLoop` 始终入队。每次只消费进入时已有的一批操作，回调中新投的操作留到下一次唤醒，防止递归执行及持续自投递饿死 IO。

新增 `--case redis-fairness`：RedisMgr 建立两条真实下游连接，在上游额度为 1、服务端时间轮 tick 为 1000ms 时发送 64+1 条命令；驱动通过 Redis MONITOR 核对后者最迟第三条执行，并检查全部回复和恢复耗时。HoldMgr 同时覆盖不 await 即启动、完成后再 await、首次/重复建连、32 个并发等待者、失败和延迟投递不内联。两项纳入 `--case all`，单、多线程分别复测。

共享 Redis 上游禁止 `SCRIPT DEBUG` 子命令（大小写不敏感，所有模式均拒绝；子命令内嵌 NUL 也拒绝，防止 C 字符串比较截断绕过），避免调试模式污染其他下游的 EVAL/PING；SCRIPT LOAD 和 EVALSHA 保留。普通 RedisMgr 回归强制使用一条上游，以两条下游验证调试命令被断开、另一条仍收到正常脚本结果，并验证重连后的 PING 与脚本加载/执行。

过滤同时覆盖 hiredis 自己识别的异步状态命令，包括 PMONITOR 和命令名内嵌 CRLF 的监控/订阅/退订变体；不能只依赖 Redis 返回未知命令错误，因为 hiredis 在收到回复前就已改变连接状态。这些命令按已有规则关闭发起的下游连接，共享上游及其他下游保持可用。

HTTP Call 与 RPC/Redis 请求一样只能 await 一次，重复等待、并发等待或等待已移走对象会抛出 std::logic_error；要重复请求须重新调用 Get/Post。建连等待对象则支持多个协程共享结果，区别在于不会重复提交业务请求。


## RPC / Redis 异常诊断

RPC 服务端解包、handler、响应序列化及发送异常通过现有日志接口记录 `rpc failure`：包含 `stage`、`token`、`call`、`sid`、`proto`、`ret` 和原始异常 `reason`。调用号尚未解出时为 0；非标准异常记为 `non-std exception`。保留原来的错误回执和任务异常钩子，日志异常不能阻止回包或连接清理。

Redis 收到错误回执时记录 `redis failure stage=server_reply`，包含进程内唯一 `request`、`sid`、命令名 `command` 和 Redis 原始错误 `reason`。迟到的错误仍记录并标记 `late=1`，不重复唤醒任务；hiredis 解析失败则记录 `reader_feed` / `reader_reply` 及原始 `errstr`，随后执行原有失败清理。

错误正文最多输出 4096 字节（转义后的长度），截断标记为 `truncated=1`；换行、NUL、其他控制字符及反斜杠按 `\xHH` 转义，保留可辨识的原始内容。命令名最多取 64 字节，不附加 key、value、Lua 脚本或请求正文。原始错误文本本身可能含业务内容，业务异常消息不要包含凭据或敏感值；这里不声称能自动识别任意错误文本中的秘密。日志正常走 stdout 和 logd，写日志抛异常时退回 stderr，不能把这条兜底误认为日志服已经落盘。
