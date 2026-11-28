# 数据库客户端

数据库接口位于 `OI::Db`，MySQL 驱动位于 `OI::Mysql`。`Client::Call` 和 `Client::Transaction` 返回 `OI::Coro::TaskResult<int32_t>`；在协程中逐条 `co_await` 并检查返回码。底层同步 SDK 在分类阻塞池中执行，等待期间 poll 可继续处理事件。时序见 [单线程](sequence-diagram/single-thread/database.md) 和 [多线程](sequence-diagram/multi-thread/database.md)。

## 构建与初始化

基础数据库库随框架构建，MySQL 驱动默认关闭，启用时设置 `-DOI_ENABLE_MYSQL=ON`。仓库为 Windows 和 Linux x64 预置 OpenSSL 与 MariaDB 客户端动态 SDK，项目自测默认使用这些 SDK，无需指定头文件和库路径；MySQL 服务端需另行提供。缺少依赖时配置失败。

使用自行提供的 SDK 时可关闭预置 SDK 并明确指定依赖。例如 Linux 下，以下路径应替换为本机实际文件；分号列表包含运行时需要随程序部署的库：

```sh
cmake -S . -B build-external \
  -DOI_USE_BUNDLED_SDK=OFF -DOI_ENABLE_MYSQL=ON \
  -DOPENSSL_INCLUDE_DIR=/opt/sdk/include \
  -DOPENSSL_SSL_LIBRARY=/opt/sdk/lib/libssl.so \
  -DOPENSSL_CRYPTO_LIBRARY=/opt/sdk/lib/libcrypto.so \
  -DOI_MYSQL_INCLUDE_DIR=/opt/mysql/include \
  -DOI_MYSQL_LIBRARY=/opt/mysql/lib/libmysqlclient.so \
  '-DOI_SDK_RUNTIME_FILES=/opt/sdk/lib/libssl.so.3;/opt/sdk/lib/libcrypto.so.3;/opt/mysql/lib/libmysqlclient.so.24'
cmake --build build-external
```

Windows 同样提供头文件路径、OpenSSL 和 MySQL 导入库路径，并在 `OI_SDK_RUNTIME_FILES` 中列出对应 DLL。运行库文件及其依赖须与目标平台和 SDK 匹配。

启用后包含 `oi_mysql.h`，通过 `OI::Mysql::MakeDriver(Config)` 创建驱动，再用驱动和连接数上限构造 `OI::Db::Client`。连接参数和凭据从应用的安全配置来源取得，不写入源码或日志；按部署需要配置 TLS CA，设置后驱动要求 TLS 并验证服务端身份。

MySQL 配置必须指定非空数据库名、1 至 65535 的有效端口，以及大于零的连接、读取和写入超时秒数；参数无效时 `MakeDriver` 抛出 `std::invalid_argument`。

客户端默认使用 `OI::Thread::BlockingType::Mysql`，必须在该类型首次启动前调用 `OI::Thread::ConfigureBlockingPool(type, threads, queueLimit)` 并检查返回码。使用其他非 Default 类别时，也要先配置对应类别；`Default` 自带默认配置。阻塞池线程数、排队上限和在途上限，与数据库客户端的连接数上限是各自独立的限制。在途上限默认 1024。阻塞池配置及关闭规则见 [协程：阻塞函数](coroutine.md#阻塞函数)。

每个客户端维护自己的连接池：请求借用空闲连接，必要时在上限内建立连接；用完且健康的连接在清理会话状态后复用，不能安全复用的连接被丢弃。连接池已满时返回 `OI_ERR_BUSY`，不会在工作线程内等待空闲连接。连接状态可能在并发请求之间变化，应用不能依赖前一次请求留下的会话状态。

停止框架或阻塞池之前，先等待该客户端所有调用真正结束，再 `co_await stClient.Close()` 并检查返回码。关闭失败时客户端仍处于关闭状态，应重试 `Close` 释放空闲连接；未成功关闭而析构时，连接清理可能退回到同步执行。

## 结构体单表操作

`oi_db_table.h` 提供 `Client` 的单表入口。标记 `db` 的协议结构生成 `constexpr DbTableName()`，默认将完整结构名转小写，不插入下划线，例如 `DbSample` 对应 `dbsample`；`rpcdata ServiceLease db(oi_service_instance)` 可指定物理表名。建表、单条和批量增删改查、按成员选列、带写结果的重载以及 `SelectAll` 均可省略表名，使用该默认值。显式表名重载保留，用于自定义表；表名只认字母、数字、下划线，最长 64，无效时返回 `OI_ERR_INVALID_ARGUMENT`。这些入口返回 `TaskResult<int32_t>`，逐条等待并检查状态码：

| 接口 | 操作 |
| --- | --- |
| `CreateTable<Row>(stError)` | 建表，列为 `NOT NULL`，使用全部 `[dbkey]` 主键 |
| `Insert(stRow, stError)` | 整行插入，主键冲突由数据库报错 |
| `Upsert(stRow, stError)` | 整行写入，主键存在时更新非主键列 |
| `Select(stRow, stError, 成员...)` | 按行中主键查询，不传成员读整行 |
| `SelectAll(vRows, stError)` | 查询整表并解码为行数组 |
| `Update(stRow, stError, 成员...)` | 按主键更新，不传成员改全部非主键列 |
| `Delete(stRow, stError)` | 按主键删除 |

单对象 `Insert`、`Upsert`、`Update`、`Delete` 可在 `Error` 前额外传 `Result&`，成功后读取 `iAffected` 和 `iInsertId`；输出须活到任务真正结束。除建表外均支持行或行数组；数组写操作使用事务，空数组成功且不执行 SQL。数组查询按输入主键逐条读取，任一失败不覆盖输出。`Select` 找不到返回 `OI_ERR_NOT_FOUND`，多行或解码失败返回 `OI_ERR_DECODE`；成功才写回。`SelectAll` 只接收输出数组，不要求主键；`CreateTable`、`Upsert`、按键的 `Select`、`Update` 和 `Delete` 要求 `[dbkey]`，更新不能指定主键列。读取和更新可通过成员指针指定部分列。

```cpp
OI::Db::Error stError;
const int32_t iStatus = co_await stMysql.Update(stRow, stError, &Row::sName, &Row::dScore);
if (iStatus != OI_OK)
{
    co_return iStatus;
}
```

列类型按字段类型推导，`[dbtype]` 可以换字节和文本列的类型，见 [模块和协议](module.md)。表的默认字符集是 `utf8mb4`。

JOIN、任意条件、范围查询和跨表操作不在这层，调用方用 `Pick` 或带参数的 SQL，表名用 `QuoteName` 加反引号前先自己校验。协议导出的逐表 SQL 可处理新增列和索引；手工 SQL 的执行仍由调用方负责。

旧版先构造语句再调用的代码，可迁移到这些 `Client` 成员；复杂查询仍使用下文的参数化 SQL。

## 协议索引与逐表 SQL

标记 `db` 的协议结构可在表内声明 `index idx_name(iField, sOther);` 或 `uniqueindex uq_name(sField);`。索引成员按声明顺序映射为数据库列；联合主键的某一个成员也可另加唯一索引，例如按区服和角色组成联合主键，同时让角色 ID 跨区服唯一。协议语法和示例见 [模块和协议](module.md)。

`[dbkey, autoincrement]` 标记自增主键成员：每表最多一个，必须是整数，且是某个索引的首列。`Insert` 和 `Upsert` 的该字段为 0 时绑定原生 SQL NULL，由数据库分配；显式非零值原样绑定。单条 `Insert` 使用带 `Result` 的重载可从 `iInsertId` 获取生成值。MySQL 的 `Upsert` 遇任一 UNIQUE 键冲突会更新已有行；需要冲突时报错时使用 `Insert`。

协议导出将每张物理表的 SQL 写入 `module/mysql_script/<表名>.sql`，框架数据定义写入其中的 `framework/` 子目录。每个文件携带逐版历史，面向 MySQL 8（实测 8.0.42 和 8.0.46）；执行账号需要会话临时表权限，普通业务不需要例程权限。先选目标库，再在客户端导入；以下方式也适用于 Windows：

```text
mysql -u 用户名 -p 目标库
mysql> source module/mysql_script/dbsample.sql
```

新增 PB 或 JSON 复杂列时，脚本为已有行填入可解码的空值。无变更时重复导出保留文件时间戳；新增列、索引或兼容的内嵌结构变化追加版本。内嵌 PB 按子类型和字段号检查，JSON 按子类型和实际持久化键检查，容器内的嵌套结构也检查。旧字段删除或类型变化会阻止导出，新增字段兼容；PB 成员只改名，或 JSON 成员改 C++ 名但保留原 JSON 别名，不影响兼容性。修改旧列、主键、已有索引或旧字段自增属性也会阻止导出；有意做不兼容变更时先备份数据，再由使用方手工删除该表 SQL 历史并重建数据库表。要保留旧值时，须先用旧版本解码，再迁移为新编码；单纯重建同型 SQL 列并回填原字节不能转换历史值。工具不执行 `DROP TABLE`。

已有历史若包含结构体却没有内嵌编码基线，先取上一次实际部署使用的协议文件，对对应 SQL 目录执行一次以下命令，再正常导出新协议。仓库自带的基线随代码提供，无需执行此步骤：

```text
python cbaselib/proto/proto_tool/main.py --baseline-mysql-nested <SQL目录> -- <该批次已部署的协议文件...>
```

静态注册的协议指定物理表 `oi_service_instance`，框架脚本导出为 `framework/oi_service_instance.sql`。外部项目若仍保留旧版自动生成的 `framework/servicelease.sql`，先备份并将旧产物移出导出目录，再导出；不要求删除数据库中的表。业务侧显式指定表名的接口保持可用。

## 执行与结果

`Client::Call(sSql, vParameters, stResult, stError)` 执行一条 SQL。MySQL 驱动使用预处理语句和 `?` 占位符，参数数量及顺序必须匹配；业务值通过绑定传递，不拼入 SQL。`OI::Db::Parameter` 用 `ParameterType` 区分 NULL、bool、有符号整数、无符号整数、浮点、文本和二进制七类。整数与浮点按原生类型绑定；文本和二进制保存在带长度的 `sBytes` 中，二进制字符串用 `Parameter::Binary(sBytes)`，也可从 `Blob` 构造。SQL 标识符不能当作值参数绑定，应由应用使用固定 SQL 或严格控制可选标识符。

```cpp
OI::Db::Result stResult;
OI::Db::Error stError;
std::vector<OI::Db::Parameter> vParameters{int64_t{1}};
const int32_t iRet = co_await stClient.Call("SELECT payload FROM example WHERE id = ?", std::move(vParameters), stResult, stError);
if (iRet != OI_OK)
{
    co_return iRet;
}
```

`Result::vRows` 是 `Row` 数组；重复列名拒绝。底层单元格是 `optional<string>`：空 optional 表示 SQL NULL，有值且长度为零表示空字节串。非 NULL 结果保留原始字节，包括内嵌 NUL；`iAffected` 和 `iInsertId` 分别提供受影响行数和插入 ID。MySQL 配置可限制结果总字节数及行数。

行来源由执行查询的驱动记录，同一结果共享。MySQL 已知列使用元数据中的原库和原表；缺列时报告能确定的来源表，纯表达式没有原表时显示 `<unknown>`。后续查询不会覆盖早前结果的来源。

读取原始结果时，用 `stRow.at("id").as_uint32()` 按列名取强类型值；`Cell` 还提供 `is_null()`、`as_string()`、`as_int8()`、`as_uint8()`、`as_int16()`、`as_uint16()`、`as_int32()`、`as_uint32()`、`as_int64()`、`as_uint64()`、`as_float()`、`as_double()` 和 `as_bool()`。缺列、NULL 或转换失败抛 `RowDecodeError`；`is_null()` 可先检查 NULL。字符串引用只在所属行存活且未修改时有效。

不希望异常时可先判断：`if (const auto stId = stRow.find("id"); stId && stId->is_uint32())` 再调用 `stId->as_uint32()`。缺列返回空 optional；各 `is_*` 按对应 `as_*` 的转换规则判断能否解码，不抛解码异常。0/1 可同时视为 bool 和数值，`is_string()` 对任何非 NULL 字节串为真。

可用 `stResult.DecodeRows(vOutput, stError, fnDecode)` 逐行返回业务对象；全部成功才替换输出。它只捕获 `RowDecodeError` 并返回解码错误，业务回调的其他异常继续传播，失败时保留旧输出。

单行单列可用 `co_await stClient.Scalar(sSql, vParameters, stValue, stError)`；结果必须恰好一行一列，非 NULL 且可解码，否则返回 `OI_ERR_DECODE`，输出不变。

`Call`、`Scalar` 和事务的输出及 `Error&` 都是借用，必须一直存活到后台工作真正结束。丢弃任务不取消工作，不能在等待超时或提前退出时释放这些对象；并发访问由调用方同步。成功时提交结果输出，失败时不要读取结果作为本次成功数据。公开 `Call` 和 `Transaction` 在提交前清空 `Error`；若阻塞池在进入驱动前拒绝，本次没有驱动错误详情。

## 事务

用 `OI::Db::Batch stBatch`，逐条 `stBatch.Add(sSql, vParameters)`，再 `co_await stClient.Transaction(std::move(stBatch), vResults, stError)`。整组在同一条借用连接上按顺序执行，成功后结果按语句顺序返回；失败时尝试回滚，连接是否可复用取决于回滚和连接状态。事务仅支持事务表上的 DML 和读取；不要放入 DDL、事务控制、存储过程或会隐式提交的语句，不承诺 DDL 能回滚。提交失败可能存在结果不确定性，应用应按业务需要处理，不自动重试写入。

## 错误与诊断

返回值是统一的 `int32_t` 状态码。驱动错误通过 `Error` 的 `sStage`、`iNativeCode`、`sSqlState` 和 `sMessage` 提供阶段及原生诊断；原生码和 SQLSTATE 可能因失败阶段不存在而为空或零。`stCleanup` 可记录连接清理失败或异常及其独立的诊断字段，调用成功时也应按需检查。MySQL 的文本使用简短安全描述，不直接输出 SDK 或服务端原始错误文本，避免泄露 SQL、参数、地址和凭据。记录诊断时只记录必要的阶段、状态码和安全文本，不记录完整请求或敏感值。

返回框架状态码的数据库协程可在边界使用 `OI_DB_TRY` / `OI_DB_CATCH`：状态码接口调用后用 `stError.ThrowIfFailed(iCode)` 转入统一处理。捕获宏对数据库异常只在边界记录一次 `LOG_ERROR` 并 `co_return` 原框架码；未知异常返回 `OI_ERR_FAIL`。需要恢复或重试时自行捕获处理。

MySQL 普通 SQL 错误返回 `OI_ERR_DB`（-18），重复键 1062 返回 `OI_ERR_EXIST`，锁等待超时返回 `OI_ERR_TIMEOUT`，死锁返回 `OI_ERR_BUSY`，连接失败或连接丢失返回 `OI_ERR_CONNECT`。原生错误号与 SQLSTATE 保留在 `Error` 中。

## 协议行

`rpcdata` 在名字后面标 `db` 才生成行映射，见 [模块和协议](module.md)。生成的方法：

- `DbFields()`：按列顺序排列的同类型字段数组，每项包含列信息和以整行为参数的编解码回调。字段操作遍历数组；成员指针查列由生成的 `DbFindField` 单独处理。
- `DbColumnList` 是反引号列名，`DbPlaceholders` 是等量的 `?`，`AppendDbParameters` 按这个顺序追加参数，用于整行写入。
- `ToDbMap()` 返回「列名 → 参数」，`FromDbMap(CellMap)` 按「列名 → 单元格」写回，只写 map 里有的列。查询结果的每行可通过 `Row::mCells` 取得 `CellMap`。
- 整份结果用 `OI::Db::FromRows(stResult, vRows)`，任一行失败返回 `OI_ERR_DECODE`，`vRows` 不变。

读回时不认识的列跳过；有 SQL NULL、重复列或某列解不开时整行返回 false，对象保持原值。

按成员指针取列：`OI::Db::ColumnOf(&Row::iId)` 返回列名，`[nodb]` 字段返回 `nullptr`。`OI::Db::Pick(stRow, stPicked, &Row::sName, &Row::dScore)` 按传入顺序给出 `sColumns`、`sMarks`、`sAssignments`（`` `a`=?,`b`=? ``）、`sConditions`（`` `a`=? AND `b`=? ``）和对应参数，手写 SQL 时用；成员不是数据库字段或重复时返回 `OI_ERR_INVALID_ARGUMENT`，输出不变。

成员指针反射目前只给标了 `db` 的结构生成。

头一层的整数、浮点、`bool`、`string`、`Bytes` 分别绑定成 `int64_t`、`uint64_t`、`double`、`string` 或 `Blob`。`bool` 写成 0 或 1。`vector`、`map` 和嵌套 `rpcdata` 默认不拆列，调用 `ToDbBlob` 收成一个 blob，读的时候用 `FromDbBlob`。blob 里是这条值自己的 PB，序号为 0。容器 blob 恰好包含一条字段（包括空容器），空 blob、多条字段和尾随字节都拒绝。旧容器 blob 与新格式不兼容，需要先用旧版本读取并迁移，相关读写端同步升级。`vector` 和 `map` 各是一条字段，不是每个元素一条。裸流里的嵌套 rpcdata 带长度前缀；同一字段号出现第二次直接拒绝。RPC 混版本、Redis 里已有的值和裸流的读写两端都受这个格式约束，旧数据要用旧端读取或先迁移，不能把新端当成平滑兼容。字段写了 `[dbjson]` 才改成 `ToJsonText` / `FromJsonText` 的 JSON 文本。数据库不穿透，blob 里的结构不用标 `db`。 `[dbjson]` 编码拒绝无效 UTF-8 和非有限浮点数；直接调用 `EncodeDbJson` 时抛 `std::invalid_argument`。合法 UTF-8 和字符串内 NUL 可经 JSON 编解码往返。单表语句构建只编码所需字段：所需字段无法编码时返回 `OI_ERR_INVALID_ARGUMENT`，输出语句不变；按主键查询或只更新其他有效字段不受该值影响。


真 MySQL 回归覆盖 29 个持久字段及 1 个 `nodb` 字段：八种定宽整数、bool、float、double、string、Bytes、PB 和 JSON 各三种复杂类型、嵌套容器及十种可配置文本/二进制列型。单条和批量 CRUD 以相反数据回读，并从 `information_schema` 核对实际列类型；通过标记为 `mysql all field types batch bad=0`。

## 本轮范围

行映射只覆盖头一层的打包和解包。文档里 MySQL 驱动的行为约束仍以接口为准；回归只有真实 MySQL 的 `mysql` 用例，检查项见 [测试](testing.md)。
