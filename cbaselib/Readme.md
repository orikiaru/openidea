# C++ 基础库

介绍一下基础目录对应功能

**proto/proto_tool**

包括模块代码生成,配置生成,组件生成,协议生成的python脚本工具


**3rd目录**

对应三方库目录，目前只有配置的toml的三方库

**base目录**

对应一些基础功能

    bytes.*         对应线程安全的写时复制的二进制流操作类 class Bytes

    bytes_stream.*  对应二进制流的序列化和反序列化操作 [目前没有做大小端转换TODO]

    config.*        对应进程toml配置的加载获取操作

    file_helper.*   对应一些文件操作，目前只支持文件存在和递归创建目录[目前创建目录不支持/\特殊符号]

    fileio.*        计划对应同步文件IO和异步文件IO操作，目前没有实现

    serialize.*     对应基础序列化结构的接口定义

    spin_lock.*     对应自旋锁和互斥锁以及Guard封装[名字是历史原因]

    task.*          对应线程池任务基类

    threadpool.*    对应线程池功能

    timer_task.*    对应定时任务


**net目录**
    poll.*              对应epollio的接口

    net_helper.*        对应一些网络接口封装

    net_socket.*        对应socket操作接口，目前只支持tcp

    channel.*           对应网络会话io的套壳[udp,tcp等目前只支持tcp]

    accept_channel.*    对应server的accept接收器

    connect_channel.*   对应client的connect的连接器

    net_channel.*       对应网络会话封装

    tcp_channel.*       对应tcp会话的io封装

    ssl_channel.*       对应tls会话的io封装[未实现]
    
    session.*           对应网络会话协议的收发控制,开启关闭

    net_session.*       对应网络协议的收发控制的通用控制
    
    session_manager.*   对应一类会话管理者封装，比如对客户端的网关会话 GateServer等

    parser.*            对应协议解码器


**proto目录**
    
    proto_session.*             对应自建协议的会话封装

    proto_session_manager.*     对应自建协议的会话管理者封装

    proto_stub_manager.*        对应自建协议的存根管理器

    protocol.*                  对应自建协议基类逻辑

    rpc.*                       对于自建远程调用协议的基类逻辑

**timer目录**

    base_timer.*                对应定时器基类

    base_timer_helper.*         对应定时器的helper接口

    base_timer_level.*          对应定时器层级操作接口

    base_timer_manager.*        对应基于定时器以及层级操作的管理者

**include目录**

    包含整个baselib所需要的基础头文件

**logger目录**

    log_client.*            对应log服务的网络组件

    log_client_session.*    对应log服务的网络组件对应的会话包装

    log_parser.*            对应log服务的网络协议解码器

    log_proto_stubs.*       对应log服务的网络协议存根注册

    log_protocol.*          对应log服务的网络协议以及协议处理接口[client实际上不处理协议,处理协议逻辑在server]

    logger.*                对应log服务对外提供的接口

**test目录**
旧有测试目录,目前无效，请使用cppdev测试



