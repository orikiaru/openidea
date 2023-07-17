# CppDev

通过使用cbaselib实现的Cpp模块化编程设计


**编译指令**

```shell
mkdir build
cd build
cmake ..
make
```

**清除指令**

```shell
make clean
```

**编译结果**

depoly/bin 		可执行的测试文件
depoly/config	对应可执行程序的配置文件，与可执行程序同名，初始由代码生成

**添加测试模块【功能模块】**

cppdev/modules/xmlconfig/module.xml 
    新增新的模块,比如网关,中心服等等

**添加测试模块的组件【网络组件】**

cppdev/modules/xmlconfig/module_component.xml 
    增加新模块组件,目前只支持增加网络组件【绑定到指定的模块下面】

**添加测试协议【每一个组件相关联的协议】**

cppdev/modules/xmlconfig/proto.xml 
    增加新协议,然后将对应协议【绑定到指定的模块组件】下

**代码生成**
    
    执行cmake后，会自行生成新增加的模块,对应模块注册的组件,模块默认配置,模块cmake文件等内容
    其中module代码生成在cppdev/modules/模块名的目录下
        其中包括 CMake文件
        和主程序 cppdev/modules/模块名/src/main.cpp
    其中module的配置生成在cppdev/deploy/config目录下
        为模块民的同名文件，以.toml为后缀
    其中module_component的代码生成在cppdev/modules/模块名的目录下
        其中包括 网络组件的头文件和cpp 生成在对应的/src 和 /inc目录下
            如：cppdev/modules/模块名/src/xxxClient.cpp 
                cppdev/modules/模块名/inc/xxxClient.h
        其中协议存根注册在 cppdev/modules/模块名/src/proto_stub.cpp
        对应注册的协议会在 cppdev/modules/模块名/protocol/*.hpp|*.hrp 下产生对应的Handle代码 【序列化反序列化公用，协议处理方式由每个模块自己决定】
            其中.hpp文件是普通的单向协议文件，不保证触达，没有超时处理
            其中.hrp文件是普通的RPC协议文件，可保证不触达有超时处理
    其中协议序列化反序列化代码生成在cppdev/modules/include目录下，所有模块公用
**代码生成的规则**   
    对于生成的协议序列化反序列化代码，每次均会重复生成，不允许修改，修改无效
    对于生成的模块和对应组件文件，不会再重复生成
    对于协议的Handle方法文件，也不会重新生成

**简单测试网络协议发送接受处理**

在cppdev/modules/xmlconfig/proto.xml 增加一个新协议，比如叫TestProtocol，声明一个字段为int的变量a

将TestProtocol注册到对应的cppdev/modules/xmlconfig/module_component.xml上，比如注册到TestClient和TestServer上

	定义好协议后:在TestServer::OnAddSession下 
		声明数据结构 TestProtocol p; 
		初始化数据 p.a=1;
		调用pSession->SendProtocol(p)即可
		重写test/protocol/testprotocol.hpp下的TestProtocol::Handle()方法，即可实现在TestServer-发送给>TestClient的协议处理
RPC协议类似,不过要Rpc不是直接定义协议序列化结构，而是分别定义请求数据结构和回复数据结构，结构一定要是RpcData类型

    Rpc为远程过程调用: request为发起者携带的数据,response为接受者回复的数据,一旦调用超时,自动调用OnTimeout()接口
    其中RpcData为特化的序列化结构, 提供给Protocol内部,和Rpc内部定义复杂的数据结构[暂时rpcdata无法互相嵌套,TODO]
