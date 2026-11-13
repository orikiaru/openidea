

from module_data_parse import ComponentType
from module_data_parse import Component
from module_data_parse import Module



class ComponentFileGen:
    def __init__(self, component: Component, module: Module) -> None:
        self.component = component
        self.module = module
        self.common_header = '#pragma once' + '\n' 
        self.common_header += '#include "oi_net.h"'  + '\n'
        self.common_header += '#include "oi_base.h"' + '\n'
        
    def GetComponentHeaderFileName(self):
        return self.component.name.lower() + '.h'
    def GetComponentCppFileName(self):
        return self.component.name.lower() + '.cpp'
    def GetIncludeFileData(self):
        return self.DoReplace('#include "__HEADER_FILE_NAME__"' + '\n')

    def DoReplace(self, file_data:str):
        header_file_name = self.GetComponentHeaderFileName()
        component_name = self.component.name
        
        file_data = file_data.replace( '__HEADER_FILE_NAME__', header_file_name)
        file_data = file_data.replace( '__COMPONENT_NAME__', component_name)
        return file_data
    
    def GenServerComponentInit(self):
        file_data = r'''
    iCode = __COMPONENT_NAME__::GetInstance().Init();
    if (iCode != OI_OK)
    {
        fprintf(stderr, "__COMPONENT_NAME__ Init failed: code=%d\n", iCode);
        return iCode;
    }
    iCode = __COMPONENT_NAME__::GetInstance().StartServer();
    if (iCode != OI_OK)
    {
        fprintf(stderr, "__COMPONENT_NAME__ StartServer failed: code=%d\n", iCode);
        return iCode;
    }   
'''     
        return self.DoReplace(file_data)
    
    
    def GenClientComponentInit(self):
        file_data = r'''
    iCode = __COMPONENT_NAME__::GetInstance().Init();
    if (iCode != OI_OK)
    {
        fprintf(stderr, "__COMPONENT_NAME__ Init failed: code=%d\n", iCode);
        return iCode;
    }
    __COMPONENT_NAME__::GetInstance().Start();
'''     
        return self.DoReplace(file_data)
    
    
    def GenServerComponentConfig(self):
        file_data = r'''
[__COMPONENT_NAME__]
ip = ""
port = ""
type = ""
'''
        return self.DoReplace(file_data)


    def GenClientComponentConfig(self):
        file_data = r'''
[__COMPONENT_NAME__]
service = ""	#留空只连下面这一台，实例号 0；填服务名就连注册表里的每一台
ip = ""
port = ""
type = ""
'''
        return self.DoReplace(file_data)
    
    def GetInitString(self)->str:
        if(self.component.GetType() == ComponentType.Server):
            return self.GenServerComponentInit()
        elif(self.component.GetType() == ComponentType.Client):
            return self.GenClientComponentInit()
        assert(False and "GetInitString Error Type")
        return ""
    
    def GetConfigString(self)->str:
        if(self.component.GetType() == ComponentType.Server):
            return self.GenServerComponentConfig()
        elif(self.component.GetType() == ComponentType.Client):
            return self.GenClientComponentConfig()
        assert(False and "GetConfigString Error Type")
        return ""

    def GenComponentHeader(self):
        if(self.component.GetType() == ComponentType.Client):
            return self.GenClientHeader()
        elif(self.component.GetType() == ComponentType.Server):
            return self.GenServerHeader()
        assert(False and "GenComponentHeader Error Type")
        return ""

    def GenComponentCpp(self):
        if(self.component.GetType() == ComponentType.Client):
            return self.GenClientCpp()
        elif(self.component.GetType() == ComponentType.Server):
            return self.GenServerCpp()
        assert(False and "GenComponentCpp Error Type")
        return ""
    
    def GenServerHeader(self):
        file_data = '''#pragma once
#include <ctime>
#include "oi_net.h"
#include "oi_rpc_coro.h"
#include "oi_singleton.h"

// 配置段名就是 GetIDString()。
class __COMPONENT_NAME__ : public OI::Rpc::ProtoSessionManager
{
	DECLARE_SINGLETON(__COMPONENT_NAME__)
	__COMPONENT_NAME__();

  public:
	virtual ~__COMPONENT_NAME__();
	int32_t Init() override;
	const std::string GetIDString() override;
	// 每秒启动一条协程，可以 co_await。上一条没结束时这一秒跳过。
	OI::Coro::Task OnSecondUpdate(time_t iNow);

  private:
	void OnAddSession(OI::Net::Session* pSession) override;
	void OnDelSession(const OI::Net::Session* pSession) override;
};
'''
        file_data = self.DoReplace(file_data)
        return file_data
    
    def GenClientHeader(self):
        file_data = '''#pragma once
#include <ctime>
#include "oi_net.h"
#include "oi_rpc_coro.h"
#include "oi_singleton.h"

// 配置段名就是 GetIDString()。段里 service 留空只连 ip/port 这一台，实例号 0；填服务名就连注册表里的每一台。
// 按实例号 Call(id, ...)，任选一台 CallAny，每台一份 CallAll。
class __COMPONENT_NAME__ : public OI::Rpc::ProtoMultiClientManager
{
	DECLARE_SINGLETON(__COMPONENT_NAME__)
	__COMPONENT_NAME__();

  public:
	virtual ~__COMPONENT_NAME__();
	int32_t Init() override;
	const std::string GetIDString() override;
	// 某台连上，连上前暂存的请求已补发，在 poll 线程。
	void OnPeerConnect(int32_t iInstanceId, sid_t iSessionId) override;
	// 每秒启动一条协程，可以 co_await。上一条没结束时这一秒跳过。
	OI::Coro::Task OnSecondUpdate(time_t iNow);

  protected:
	// 某台断开，在 poll 线程。这台的在途调用已经回 OI_ERR_CONNECT。
	void OnPeerDisconnect(int32_t iInstanceId, sid_t iSessionId) override;
};
'''
        file_data = self.DoReplace(file_data)
        return file_data
    
    def GenClientCpp(self):
        file_data ='''#include "__HEADER_FILE_NAME__"

__COMPONENT_NAME__::__COMPONENT_NAME__()
{
}

__COMPONENT_NAME__::~__COMPONENT_NAME__()
{
}

int32_t __COMPONENT_NAME__::Init()
{
	return OI::Rpc::ProtoMultiClientManager::Init();
}

const std::string __COMPONENT_NAME__::GetIDString()
{
	return "__COMPONENT_NAME__";
}

void __COMPONENT_NAME__::OnPeerConnect(int32_t iInstanceId, sid_t iSessionId)
{
}

void __COMPONENT_NAME__::OnPeerDisconnect(int32_t iInstanceId, sid_t iSessionId)
{
}

OI::Coro::Task __COMPONENT_NAME__::OnSecondUpdate(time_t iNow)
{
	co_return;
}
'''
        file_data = self.DoReplace(file_data)
        return file_data
          
    def GenServerCpp(self):
        file_data ='''#include "__HEADER_FILE_NAME__"

__COMPONENT_NAME__::__COMPONENT_NAME__()
{
}

__COMPONENT_NAME__::~__COMPONENT_NAME__()
{
}

int32_t __COMPONENT_NAME__::Init()
{
	return OI::Rpc::ProtoSessionManager::Init();
}

const std::string __COMPONENT_NAME__::GetIDString()
{
	return "__COMPONENT_NAME__";
}

OI::Coro::Task __COMPONENT_NAME__::OnSecondUpdate(time_t iNow)
{
	co_return;
}

void __COMPONENT_NAME__::OnAddSession(OI::Net::Session* pSession)
{
}

void __COMPONENT_NAME__::OnDelSession(const OI::Net::Session* pSession)
{
}
'''
        file_data = self.DoReplace(file_data)
        return file_data
