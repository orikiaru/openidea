# 待办

## hold 的 redis late ping 偶发超时

- 现状：`hold` 的 `CheckRedisLateReply` 先发 `BLPOP hold-late 3`，客户端 2 秒判超时，接着在同一条连接上发 PING。正常 PONG 排在 BLPOP 的 nil 后面，约 1 秒到（`late ping ok=1 wall_ms≈900~1100`）。linux-mt 上偶发 `late ping ok=0 status=1 reply_type=-1 wall_ms≈2010`，PONG 在 PING 自己的 2 秒超时内没回来，`hold done bad=1`。2026-11-11 单独重跑三次挂一次，无并行负载时也会挂；Windows 和 linux-st 没见过。
- 诊断：2026-11-11 在隔离的 Linux MT 路复跑四次均通过，迟到 PING 分别耗时 1040、910、844、849 毫秒。后三次通过 Redis MONITOR 观察到 BLPOP 和 PING 在同一上游连接，PING 在 BLPOP 后约 3.01～3.08 秒执行；这不能证明回复已送达客户端。未找到原失败日志，尚未抓到失败时的时间线，根因未确认。
- 方向：先抓一次失败时 redisserver 和 Redis 两边的时间线，确认 PONG 是晚到、被丢，还是被记到超时那条命令上。再决定是修 Redis 客户端/redisserver 的迟到回复处理，还是放宽用例的时间余量。

## Windows ST disk-full 清理通知 helper 偶发退出 120（2026-11-12）

- 现状：本轮两次 Windows ST 全量均为 20/21；disk-full 已成功检测 ENOSPC，但清理通知 helper 退出码为 120。增加 stdout/stderr 捕获和失败诊断后定点运行通过，模拟退出 120 仍会报错；根因尚未修复或确认。
- 证据：本地结果 `windows-st-latest` 和 `windows-st-disk-diagnostic`。
- 方向：保留现有证据，下次失败时捕获 stderr，核实控制台与标准流生命周期。禁止吞掉退出码 120 或放宽断言。

## C++ 实际命名 AST 扫描缺依赖（2026-11-12）

- 现状：缺少 Python clang/libclang，实际命名 AST 扫描受阻。命名规则自身 9 项测试通过，继承检查和格式检查通过，但不能代替实际扫描。
- 方向：持久安装所需依赖，使用现有 `tools/check_field_names.py` 根据编译数据库执行扫描，并区分存量问题。

## 底层框架少用嵌套模板，协议生成优先（2026-11-13）

- 需求：底层框架能不用多层嵌套模板的地方不用。协议生成是大头：每条 rpcdata 的每个字段都要做一次重载决议和模板实例化，容器和嵌套结构再往下套一层。协议越多，编译量越大。数据库字段表已改为同类型描述及普通循环。对外字节、JSON、列顺序、主键、`[dbtype]`、`[dbjson]`、NULL 和按成员取列的结果保持不变。
- 现状：生成器已经按字段写出直线调用，但每个调用都落到头文件模板上。`GetPbSerialize` 写出 `os.WritePb(序号, 字段)`，`GetPbDeserialize` 写出 `os.ReadPb`。定宽类型走 `enable_if<PbFixedTrait<T>>`，`vector<T>` 和 `map<K, V>` 再递归实例化元素类型；JSON 的整数用 `enable_if_t`，rpcdata 用 `decltype(FromJson)` 做 SFINAE，`vector` / `map` 同样递归。字段每多一层容器或嵌套结构，就多一套实例。数据库字段表使用 `Field<行>` 数组，编码解码回调接收整行，成员指针查找由生成的 `DbFindField` 单独处理。样例在 `modules/include/oi_rpcdata_dbsample.h` 的 `serialize`、`FromJson` 和 `DbFields`。对应实现在 `cbaselib/base/inc/oi_bytes_stream.h`、`cbaselib/proto/inc/oi_json.h`、`cbaselib/db/inc/oi_db_row.h`、`cbaselib/db/inc/oi_db_table.h`，生成在 `cbaselib/proto/proto_tool/serialize_data_file_gen.py`。
- 方向：生成器已经知道每个字段的具体类型，按标量、string、Bytes、vector、map、嵌套 rpcdata 直接写出读写，容器在生成代码里展开。框架侧对这几种类型留普通重载，不再每个字段做 `enable_if`、`decltype` 和递归模板。其余框架新增和改动按同一条来，不靠再套一层类型推断复用。改生成器和对应头，生成头不要手改。字节、JSON 和 mysql 行映射用现有回归锁住。

## 其他语言的序列化

- 需求：序列化出去的标准数据结构统一为小端。非 C++ 的对端按这份标准读写 `modules/protoconfig` 里的协议。HTTP 对接先用已有 JSON；要做 RPC，或把 rpcdata 放进 Redis 的对端，必须用同一套二进制。先只生成一种真正要接的语言。现在的小端机器上，已经写出的包和 Redis 里的 rpcdata 字节必须保持不变。
- 现状：二进制由 `proto_tool` 生成，不是标准 Protobuf，不能用 `protoc` 解析。一条字段是 `uint32` 字段号、`uint32` 长度、裸内容。`vector` 用同一个字段号重复多条。不认识的字段号按长度跳过，缺字段保持原值。序列化层已统一小端（字段头、长度前缀、定宽整数、枚举、float、double），大端主机按字节翻转，`mysql` 用例的离线行映射检查用固定字节样例锁住输出。MySQL 客户端已经接入，数据库行映射见 `docs/database.md`；schema migration 还没有。其余 rpcdata 已有 JSON；标了 `key` 的结构不生成 JSON。Web 的 TLS 不进框架，需要时在外面用 nginx 反代到现有 HTTP。
- 方向：然后把这份标准写成不依赖 C++ 的说明。schema 仍是 `protoconfig`，在 `proto_tool` 上加一种语言的后端，不另起第二套协议。只做 Web 时先生成 JSON 编解码；要接 RPC 或 Redis 里的 rpcdata，再生成二进制。
