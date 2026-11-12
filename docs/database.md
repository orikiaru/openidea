# 数据库客户端

数据库接口位于 `OI::Db`，MySQL 驱动位于 `OI::Mysql`。`Client::Call` 和 `Client::Transaction` 返回 `OI::Coro::TaskResult<int32_t>`；在协程中逐条 `co_await` 并检查返回码。底层同步 SDK 在分类阻塞池中执行，等待期间 poll 可继续处理事件。

## 构建与初始化

基础数据库库随框架构建，MySQL 驱动默认关闭。启用时设置 `-DOI_ENABLE_MYSQL=ON`；CMake 会查找 MySQL 或 MariaDB 客户端头文件和库，也可用 `-DOI_MYSQL_INCLUDE_DIR=<头文件目录>`、`-DOI_MYSQL_LIBRARY=<库文件路径>` 明确指定。缺少依赖时配置失败。启用后包含 `oi_mysql.h`，通过 `OI::Mysql::MakeDriver(Config)` 创建驱动，再用驱动和连接数上限构造 `OI::Db::Client`。连接参数和凭据从应用的安全配置来源取得，不写入源码或日志；按部署需要配置 TLS CA，设置后驱动要求 TLS 并验证服务端身份。

MySQL 配置必须指定非空数据库名、1 至 65535 的有效端口，以及大于零的连接、读取和写入超时秒数；参数无效时 `MakeDriver` 抛出 `std::invalid_argument`。

客户端默认使用 `OI::Thread::BlockingType::Mysql`，必须在该类型首次启动前调用 `OI::Thread::ConfigureBlockingPool(type, threads, queueLimit)` 并检查返回码。使用其他非 Default 类别时，也要先配置对应类别；`Default` 自带默认配置。阻塞池线程数和排队上限，与数据库客户端的连接数上限是两套独立限制。阻塞池配置及关闭规则见 [协程：阻塞函数](coroutine.md#阻塞函数)。

每个客户端维护自己的连接池：请求借用空闲连接，必要时在上限内建立连接；用完且健康的连接在清理会话状态后复用，不能安全复用的连接被丢弃。连接池已满时返回 `OI_ERR_BUSY`，不会在工作线程内等待空闲连接。连接状态可能在并发请求之间变化，应用不能依赖前一次请求留下的会话状态。

停止框架或阻塞池之前，先等待该客户端所有调用真正结束，再 `co_await stClient.Close()` 并检查返回码。关闭失败时客户端仍处于关闭状态，应重试 `Close` 释放空闲连接；未成功关闭而析构时，连接清理可能退回到同步执行。

## 执行与结果

`OI::Db::Statement` 由 SQL 文本 `sSql` 和参数数组 `vParameters` 构成。MySQL 驱动使用预处理语句和 `?` 占位符，参数数量及顺序必须匹配；业务值通过绑定传递，不拼入 SQL。`OI::Db::Parameter` 支持 `nullptr`、`int64_t`、`uint64_t`、`double`、`std::string` 和 `OI::Db::Blob`。SQL 标识符不能当作值参数绑定，应由应用使用固定 SQL 或严格控制可选标识符。

```cpp
OI::Db::Statement stStatement;
stStatement.sSql = "SELECT payload FROM example WHERE id = ?";
stStatement.vParameters.emplace_back(int64_t{1});
OI::Db::Result stResult;
OI::Db::Error stError;
const int32_t iRet = co_await stClient.Call(std::move(stStatement), stResult, stError);
if (iRet != OI_OK)
{
    co_return iRet;
}
```

`Result::vColumns` 保存列名，`vRows` 按行和列保存 `optional<Blob>`：空 optional 表示 SQL NULL；有值且长度为零表示空字节串，两者不同。非 NULL 的结果保留为字节数组，包括内嵌 NUL；应用按列约定自行解码，不假设结果已转换成 C++ 数字。`iAffected` 和 `iInsertId` 分别提供受影响行数和插入 ID。MySQL 配置还可限制结果总字节数及行数。

`Call` 的 `Result&`、`Error&` 和 `Transaction` 的 `vector<Result>&`、`Error&` 都是借用输出，必须一直存活到后台工作真正结束。丢弃任务不取消工作，不能在等待超时或提前退出时释放这些对象；并发访问由调用方同步。成功时提交结果输出，失败时不要读取结果作为本次成功数据。调用可能在进入驱动前被阻塞池拒绝，此时不能假设错误详情一定更新。

## 事务

`Transaction` 接收一组 `Statement` 和结果数组，整组在同一条借用连接上执行；成功后按语句顺序返回结果。失败时尝试回滚，连接是否可复用取决于回滚和连接状态。事务仅支持事务表上的 DML 和读取；不要放入 DDL、事务控制、存储过程或会隐式提交的语句，不承诺 DDL 能回滚。提交失败也可能存在结果不确定性，应用应按业务需要处理，不自动重试写入。

## 错误与诊断

返回值是统一的 `int32_t` 状态码。驱动错误通过 `Error` 的 `sStage`、`iNativeCode`、`sSqlState` 和 `sMessage` 提供阶段及原生诊断；原生码和 SQLSTATE 可能因失败阶段不存在而为空或零。`stCleanup` 可记录连接清理失败或异常及其独立的诊断字段，调用成功时也应按需检查。MySQL 的文本使用简短安全描述，不直接输出 SDK 或服务端原始错误文本，避免泄露 SQL、参数、地址和凭据。记录诊断时只记录必要的阶段、状态码和安全文本，不记录完整请求或敏感值。

## 协议行

`rpcdata` 在名字后面标 `db` 才生成行映射，见 [模块和协议](module.md)。生成三个方法：`DbColumnList` 是反引号列名，`DbPlaceholders` 是等量的 `?`，`AppendDbParameters` 按这个顺序追加参数。`FromDbRow` 用结果里的列名把一行填回来。

头一层的整数、浮点、`bool`、`string`、`Bytes` 分别绑定成 `int64_t`、`uint64_t`、`double`、`string` 或 `Blob`。`bool` 写成 0 或 1。`vector`、`map` 和嵌套 `rpcdata` 默认不拆列，调用 `ToDbBlob` 收成一个 blob，读的时候用 `FromDbBlob`。blob 里是这条值自己的 PB，序号为 0。字段写了 `[dbjson]` 才改成 `ToJsonText` / `FromJsonText` 的 JSON 文本。数据库不穿透，blob 里的结构不用标 `db`。SQL 仍由调用方写，表名不从协议生成。schema migration 尚未实现。

## 本轮范围

行映射只覆盖头一层的打包和解包。文档里 MySQL 驱动的行为约束仍以接口为准；`db` 回归用假驱动覆盖客户端，并检查 `db proto map bad=0`。真实 MySQL 仍是单独的 `mysql` 用例。
