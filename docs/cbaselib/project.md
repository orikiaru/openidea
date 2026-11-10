# 工程

外部工程 include [`framework.cmake`](../../framework.cmake#L1)。它编三方库和 `cbaselib`，并生成 net 的内部工作协议。进程模块不在这里。

协议和进程仍走自己的 xml，见 [模块和协议](../module.md)。

## 接口

| 用途 | 声明 |
| --- | --- |
| 引入基础库 | [`framework.cmake`](../../framework.cmake#L1) |
| 单线程库名 | [`BASE_LIBS`](../../framework.cmake#L38) |
| 多线程库名 | [`BASE_M_LIBS`](../../framework.cmake#L37) |

```cmake
include("${CMAKE_CURRENT_SOURCE_DIR}/openidea/framework.cmake")
```

逻辑简单的服务端链 `*_static` 并定义 `_SINGLE_THREAD_`。要接界面的客户端链 `*_m_static`。

> **注意：** 路径按 `framework.cmake` 自己的目录算。不要在业务工程里再 `add_subdirectory` 一遍 `cbaselib`。

> **注意：** 业务的 `modules/include` 由调用方自己加进头文件目录。`framework.cmake` 不带它。
