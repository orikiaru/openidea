# 模块和协议

三个 xml 是生成输入，都在 `modules/xmlconfig`。

| 文件 | 作用 |
| --- | --- |
| `module.xml` | 一个 `<module>` 是一个进程 |
| `module_component.xml` | 模块下的网络组件，`type` 为 `Client` 或 `Server`，上面挂 rpc |
| `proto.xml` | `rpcdata` 和 `rpc`。`rpc` 写 `request`、`response`、`id`、`res_id`、`timeout` |

字段按声明顺序从 0 编号。要跳号时写 `seq`。

cmake 调用 `cbaselib/proto/proto_tool/main.py`，参数是这三个 xml、`modules/include`、`modules`、`deploy`。

## 什么会重写

`modules/include` 里的序列化头和 `rpc_ids.h` 每次 cmake 都重写。改这些文件无效。

下面这些只在文件不存在时生成，已有文件会留下：

- `modules/<name>/src/main.cpp`
- `modules/<name>/CMakeLists.txt`
- 组件的 `inc/*.h` 和 `src/*.cpp`
- `deploy/config/<name>.toml`

根目录 `CMakeLists.txt` 里写死了要编的模块名。新模块除了写 xml，还要把名字加进这个列表，否则不会编。

```cmake
foreach(_mod logd ipcclient ipcserver)
    if(EXISTS ${MODULES_PATH}/${_mod}/CMakeLists.txt)
        add_subdirectory(${MODULES_PATH}/${_mod})
    endif()
endforeach()
```

## 加一个回声协议

在 `proto.xml` 增加请求和回复两个 `rpcdata`，再加一条 `rpc`，请求和回复各一个协议号。

在 `module_component.xml` 里把这条 rpc 挂到对应的 Client 和 Server 组件上。

服务端在组件 `Init` 里 `HandleRpc`，客户端在连上之后 `co_await RpcCall`。写法见 [协程](coroutine.md)。

现成例子是 `AllEcho`：`ipcserver` 把请求原样放进回复，`ipcclient` 连上后按条发出去并核对回复。
