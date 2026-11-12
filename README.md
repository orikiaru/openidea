# openidea

C++20 的模块化网络进程。基础库在 `cbaselib`，业务进程从 `modules/protoconfig` 生成骨架。同一条协程任务可以跑在单线程，也可以跑在多线程。

## 目录

| 路径 | 内容 |
| --- | --- |
| `framework.cmake` | 基础库。不含进程模块，根 `CMakeLists.txt` 和外部工程都 include 它 |
| `cbaselib` | 线程池、网络、协议、定时器、日志客户端 |
| `modules/protoconfig` | 模块、组件、协议的生成输入 |
| `modules/include` | 每次 cmake 重写的序列化代码 |
| `modules/<name>` | 一个进程。名单在 `module.proto`：`logd`、`ipcclient`、`ipcserver`、`redisserver`、`webserver`、`webclient`、`registry` |
| `deploy/bin` | 可执行文件 |
| `deploy/config` | 与可执行文件同名的 toml |
| `docs` | 说明 |

## 编译

CMake 3.10、Python 3，编译器用 GCC 11 或更高。这套协程调度在 GCC 10 上不能用。

```shell
mkdir build
cd build
cmake ..
make
```

根 `CMakeLists.txt` 先 include `framework.cmake`，编好三方库和 `cbaselib`，再跑 `cbaselib/proto/proto_tool/main.py` 生成测试模块。可执行文件在 `deploy/bin`。外部工程同样 include `framework.cmake`，用自己的 xml 生成自己的模块，见 [模块和协议](docs/module.md)。给 clangd 用的编译数据库在 `build-clangd/compile_commands.json`，仓库根的 `.clangd` 指向这个目录。Visual Studio 的 `build-win` 不会写出这份数据库。

模块默认多线程，链接 `*_m_static`。要单线程时，把该模块 `CMakeLists.txt` 里的 `SINGLE_THREAD` 改成 `TRUE`，会改链 `*_static` 并定义 `_SINGLE_THREAD_`。`logd` 受 logmanager 限制，只能多线程。

## 运行

在 `deploy/bin` 下启动，配置读 `../config/<进程名>.toml`。

先起 `logd`，再起 `ipcserver`，再起 `ipcclient`。默认 ipc 端口 `17981`，日志端口 `19995`，地址是 `127.0.0.1`。

## 说明

- [基础库](docs/cbaselib.md)（[按功能接到接口](docs/cbaselib.md#接入)）
- [协程](docs/coroutine.md)
- [模块和协议](docs/module.md)
- [定时器](docs/timer.md)
- [服务注册](docs/registry.md)
- [时序图](docs/sequence-diagram/README.md)
