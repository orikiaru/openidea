# 服务名单来源改造方案

状态：方案，未实施。现有实现见 `docs/registry.md`。

## 1. 目标

服务名单目前只有一种来源：`registry` 进程加 Redis 的主动注册（登记、心跳、过期）。改造后支持三种来源，按配置切换：

| 模式 | 名单从哪来 | 适用 |
| --- | --- | --- |
| `registry` | 现有 registry 进程，进程启动时自己登记、按秒心跳 | 容器、动态扩缩容、端口不固定 |
| `static` | toml 里写死的名单 | 固定部署，不想起 registry 和 Redis |
| `db` | 数据库里一张名单表，运维维护，进程启动时抢占自己那一行 | 固定部署、名单集中管理、需要防重复启动 |

另外让配置支持环境变量覆盖，同一个镜像起多个副本时可以按副本注入 `Service.id`、`advertise_ip`。

不变的部分：路由层和会话管理只认 `ServiceTable`（`cbaselib/base/inc/oi_registry.h`）。`Snapshot`、`Watch`、`WatchList`、`Apply`，按名字的 `RpcCall`、`CallAny`、`CallMany`、`CallAll`，`ProtoMultiClientManager` 的整份名单，接口和语义都不改。改的只是给 `ServiceTable` 喂数据的那一头。

## 2. 分步

1. 配置支持环境变量覆盖。独立，先做。
2. 来源开关和 `static` 模式。
3. `db` 模式，包括启动抢占和续租。

每步单独提交，单独跑回归。

## 3. 第 1 步：配置支持环境变量覆盖

### 规则

- 变量名：`OI_CFG__<段名>__<键名>`，段名、键名和 toml 里的写法完全一致，大小写敏感。例：`OI_CFG__Service__id=3`、`OI_CFG__Service__advertise_ip=10.0.0.7`、`OI_CFG__IpcServer__port=17981`。
- 只覆盖两层键（`段.键`），和现有配置的用法一致。名字里出现多于一个 `__` 分隔，或段名、键名为空，启动报错退出，不静默忽略。
- 值按 toml 字面量解析：`3` 是整数，`true` 是布尔，`["a","b"]` 是数组；解析失败时整体当字符串。这样 `GetAs<int32_t>` 和 `GetAs<std::string>` 都能拿到正确类型，不用调用方改代码。
- 覆盖发生在 `Config::Parse` 合并完文件之后。`Parse` 被调多次时每次都重新套一遍，结果相同。
- 启动时打一行日志，列出被覆盖的键名，不打值（配置里有 MySQL 密码这类内容）。

### 落点

- `cbaselib/base/src/oi_config.cpp` 的 `Parse` 末尾。遍历环境变量需要平台差异：POSIX 用 `environ`，Windows 用 `GetEnvironmentStringsW` 转 UTF-8。放进 `cbaselib/port` 或 `oi_config.cpp` 的平台分支里，不散到调用方。
- 不新增读取接口，`GetAs`、`TryGetConfig` 照旧。

### 验收

- 新增回归检查：`run_regressions.py` 启动 ipcclient 时设置 `OI_CFG__Test__env_probe=42` 和一个字符串键，ipcclient 读出并打印，脚本比对。toml 里同名键写别的值，确认被覆盖。
- 非法变量名（`OI_CFG__Service`、`OI_CFG____id`）启动失败、退出码非零。
- 纯配置逻辑，和线程无关，Windows、Linux 各跑一遍即可。

## 4. 第 2 步：来源开关和 `static` 模式

### 配置

```toml
[Registry]
# registry | static | db。不写时：配了 Registry.ip 就是 registry，否则不启用名单。
mode = "static"
# 一行一个实例：名字 实例号 ip 端口 [分区]。格式和 registry 里存的值一致，多一个名字和实例号在前面。
instances = [
    "gameserver 1001 10.0.0.1 17981 1001",
    "gameserver 1002 10.0.0.2 17981 1002",
    "ipcserver 1 127.0.0.1 17981",
]
```

不写 `mode` 时行为和现在完全一样，老配置不用改。

名字、分区沿用现有校验：字母、数字、`_`、`-`，实例号是正整数。同名同实例号重复、格式不对，启动报错退出。

### `RegistryClient` 里怎么分流

内部加一个模式枚举，启动时读一次。三个入口按模式分流，其余不动：

| 入口 | `registry` | `static` | `db` |
| --- | --- | --- | --- |
| `Configured()` | 有 `Registry.ip` | 真 | 真 |
| `PullSync` / `Lookup` | 现状：RPC 拉取 | 从内存名单里挑这个名字，`Store` 进缓存，不走网络 | 查表，见第 5 节 |
| `Publish` | 现状：同步登记 | 不登记。本进程的 `Service.name` + `Service.id` 不在名单里，或名单地址和本进程对外地址不一致时打 `LOG_ERROR`，不拒绝启动 | 抢占自己那一行，见第 5 节 |
| `OnSecondUpdate` | 现状：连接、心跳、拉取 | `m_bPullWanted` 或到 `pull_ms` 时，把关注的名字从内存名单 `Apply` 一次，不走网络 | 续租、按 `pull_ms` 查表后 `Apply` |

`static` 和 `db` 的实现分别放进 `cbaselib/proto/src/oi_registry_static.cpp`、`oi_registry_db.cpp`，`RegistryClient` 以成员对象持有，不加基类。`registry` 模式的现有代码原样保留。

`StartServer` 里调 `Publish` 的地方不用改，模式判断在 `Publish` 内部。

### 和 `registry` 模式的语义差别（要写进文档）

- 没有在线检测。名单里的实例一直算"在线"，挂了的实例靠建连失败暴露：`CallAny` 选中它会拿到 `OI_ERR_CONNECT`，`ProtoMultiClientManager` 会对它不停重连。
- 单台跟随（`Watch`，`ProtoClientManager` 配 `service`）固定选实例号最小的那台。它挂了不会切到下一台，因为名单没变。需要冗余的调用方用 `ProtoMultiClientManager` 或 `CallAny`。是否要加"建连连续失败 N 次就临时摘掉"放到后续，不在这一步做。
- 没有防重复启动。同一台机器上端口冲突会让后起的绑定失败；跨机器同一个实例号起两份发现不了。
- 名单运行期不变，改名单要重启。不做配置热加载。

### 验收

- 新增回归：临时 toml 设 `mode = "static"`，不起 registry 和 Redis，跑 `hold` 里依赖注册的那几组（`Lookup`、按名字 `RpcCall`、`CallMany`、`ProtoMultiClientManager` 名单）。期望结果和 registry 模式一致。
- `Publish` 自检：名单里缺本进程、地址不一致，各打一次 `LOG_ERROR`，进程照常起来。
- 名单格式错误、重复实例号，启动失败。
- 不写 `mode` 的老配置，现有 `--case all` 全部照旧通过。
- 有行为改动，Windows、Linux 的单线程、多线程四组都跑。

## 5. 第 3 步：`db` 模式

### 表

名单表由运维维护地址，进程只动租约列。框架提供建表语句，表结构用 `framework.proto` 里一条带 `db` 标记的 rpcdata 描述，生成器出建表语句：

| 列 | 类型 | 说明 |
| --- | --- | --- |
| `name` | `VARCHAR(64)`，主键 | 服务名 |
| `id` | `INT`，主键 | 实例号 |
| `ip` | `VARCHAR(64)` | 对外地址 |
| `port` | `VARCHAR(8)` | 对外端口 |
| `zone` | `VARCHAR(32)` | 分区，全局服为空 |
| `owner` | `VARCHAR(128)` | 当前持有者：主机名 + pid + 启动时间，空表示无人 |
| `lease_until` | `BIGINT` | 租约到期，数据库时钟的毫秒 |

联合主键 `name` + `id`，合计 64×4 + 8 字节，在 3072 字节上限内。

### 启动抢占（防重复启动）

`Publish` 时执行一条条件更新，用数据库时钟，不用本机时钟：

```sql
UPDATE oi_service_instance
SET owner = ?, lease_until = 当前毫秒 + expire_ms
WHERE name = ? AND id = ? AND (owner = '' OR owner = ? OR lease_until < 当前毫秒)
```

- 影响 1 行：抢到，继续检查这一行的 `ip`/`port` 和本进程对外地址是否一致，不一致打 `LOG_ERROR` 并拒绝启动（地址错了别人连不上，早失败）。
- 影响 0 行：查这一行。行不存在报"名单里没有本实例"，拒绝启动；行存在说明被别人持有，返回 `OI_ERR_EXIST`，拒绝启动。和现在 registry 模式的语义一致。

### 续租和下线

- `OnSecondUpdate` 按 `heartbeat_ms` 用同样的条件更新续租，条件改成 `owner = 自己`。影响 0 行说明租约被别人抢了，按现在 `m_bLost` 的处理：停止续租，打错误日志。
- 正常退出时通过 `ThreadPool::AddStopHandler` 清空自己的 `owner`。异常退出靠租约过期，最慢 `expire_ms + pull_ms` 被调用方看到下线。

### 拉取

- 按 `pull_ms` 查关注的名字，只取租约有效的行：`WHERE name IN (...) AND lease_until >= 当前毫秒`。这样没在跑的实例会从名单里掉出去，单台跟随也能切到下一台，补上 `static` 模式缺的在线检测。
- 查询失败时缓存保持原状，下一秒重试，和 registry 模式"拉取失败不判下线"一致。

### 数据库访问

- 用现有 MySQL 客户端（`cbaselib/db`）。现有 `BuildSelectByKey`、`BuildUpdateByKey` 只支持按完整主键操作，上面的 `IN` 查询和条件更新直接写 `Statement`，参数走绑定，不拼字符串。
- 数据库调用都是同步 SDK，必须经 `RunBlocking` 放到阻塞池，`OnSecondUpdate` 里 `co_await`。按 AGENTS.md 的规则：lambda 只按值捕获，不捕获 `this` 和引用，不在阻塞回调里调任何异步接口。
- 起服的 `Publish`、`PullSync` 是同步接口，直接同步执行，不能在 poll 回调里调，和现在一样。
- 连接配置放在 `[Registry]` 下的数据库段，和业务库分开配置，可以指向同一个库。

### 验收

- 只在 Linux 有 mysqld 的环境跑，和现有 `mysql` 用例一样显式开启，不进 `--case all`。
- 用例：同一实例号起第二个进程，返回 `OI_ERR_EXIST` 并退出；第一个进程强杀后，租约过期再起能抢到；名单地址和对外地址不一致时拒绝启动；实例下线后调用方在 `expire_ms + pull_ms` 内看不到它；数据库暂停期间缓存保持，恢复后续租和拉取恢复。
- Linux 单线程、多线程都跑；Windows 只编译，不跑数据库用例。

## 6. 文档

- `docs/registry.md`：开头说明三种模式，现有内容归到 registry 模式下；补 `static`、`db` 的语义差别。
- `docs/testing.md`：新增的 static 和 db 用例。
- `docs/cbaselib.md` 的配置段：环境变量覆盖规则。
- `todo.md`：`static` 模式下的"建连失败临时摘除"作为后续项。

## 7. 待定

- 环境变量命名：`OI_CFG__段__键` 是否可以。
- `static` 名单格式：用字符串数组（和 registry 存值一致，实现最简单），还是 toml 的表数组（可读性好，需要给 `Config` 加读表数组的接口）。
- `db` 模式下地址是否只允许运维维护，还是另开一个开关允许进程自己写入地址（那样就接近 registry 模式，主要给容器用）。
- 不写 `mode` 时的默认值是否保持现状（有 `Registry.ip` 就是 registry）。
