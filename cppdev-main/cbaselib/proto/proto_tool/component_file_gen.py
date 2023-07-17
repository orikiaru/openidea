

from module_data_parse import ComponentType
from module_data_parse import Component
from module_data_parse import Module



class ComponentFileGen:
    def __init__(self, component: Component, module: Module) -> None:
        self.component = component
        self.module = module
        self.common_header = '#pragma once' + '\n' 
        self.common_header += '#include "net_header.h"'  + '\n'
        self.common_header += '#include "base_header.h"' + '\n'
        
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
    if(!__COMPONENT_NAME__::GetInstance().Init())
    {
        printf("__COMPONENT_NAME__ Init error\n");
        return -1;
    }
    if (!__COMPONENT_NAME__::GetInstance().StartServer())
    {
        printf("__COMPONENT_NAME__ StartServer error\n");
        return -1;
    }   
'''     
        return self.DoReplace(file_data)
    
    
    def GenClientComponentInit(self):
        file_data = r'''
    if(!__COMPONENT_NAME__::GetInstance().Init())
    {
        printf("__COMPONENT_NAME__ Init error\n");
        return -1;
    }
    __COMPONENT_NAME__::GetInstance().StartClient();
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
#include "protocol.h"
#include "rpc.h"
#include "net_header.h"

class __COMPONENT_NAME__ : public ProtoSessionManager
{
	static __COMPONENT_NAME__ inst;
public:
	__COMPONENT_NAME__(){}
	virtual ~__COMPONENT_NAME__();
	bool Init();
	static __COMPONENT_NAME__& GetInstance() { return inst; }
	virtual const std::string GetIDString();
private:
	virtual void OnAddSession(Session* pSession);
	virtual void OnDelSession(const Session* pSession);
};        
'''
        file_data = self.DoReplace(file_data)
        return file_data
    
    def GenClientHeader(self):
        file_data = '''#pragma once
#include "protocol.h"
#include "rpc.h"
#include "net_header.h"

class __COMPONENT_NAME__ : public ProtoClientManager
{
	static __COMPONENT_NAME__ inst;
public:
	__COMPONENT_NAME__(){}
	virtual ~__COMPONENT_NAME__();
	bool Init();
	static __COMPONENT_NAME__& GetInstance()  { return inst; }
	virtual const std::string GetIDString();
private:
	virtual void OnClientConnect(Session* pSession);
	virtual void OnClientDisconnect(const Session* pSession);
	virtual void OnAbortSession();
};
'''
        file_data = self.DoReplace(file_data)
        return file_data
    
    def GenClientCpp(self):
        file_data ='''#include "__HEADER_FILE_NAME__"

__COMPONENT_NAME__ __COMPONENT_NAME__::inst;

__COMPONENT_NAME__::~__COMPONENT_NAME__()
{

}

bool __COMPONENT_NAME__::Init()
{
	return SessionManager::Init();
}

const std::string __COMPONENT_NAME__::GetIDString()
{
    return "__COMPONENT_NAME__";
}


void __COMPONENT_NAME__::OnClientConnect(Session* pSession)
{
    
}

void __COMPONENT_NAME__::OnClientDisconnect(const Session* pSession)
{

}

void __COMPONENT_NAME__::OnAbortSession()
{

}        
'''
        file_data = self.DoReplace(file_data)
        return file_data
          
    def GenServerCpp(self):
        file_data ='''#include "__HEADER_FILE_NAME__"

__COMPONENT_NAME__ __COMPONENT_NAME__::inst;

__COMPONENT_NAME__::~__COMPONENT_NAME__()
{

}

const std::string __COMPONENT_NAME__::GetIDString()
{
    return "__COMPONENT_NAME__";
}

bool __COMPONENT_NAME__::Init()
{
	return SessionManager::Init();
}

void __COMPONENT_NAME__::OnAddSession(Session* pSession)
{
    
}

void __COMPONENT_NAME__::OnDelSession(const Session* pSession)
{

}

'''
        file_data = self.DoReplace(file_data)
        return file_data
    
    
   
   
#下面暂时没有用,有些想多了 

    def GetMemberDecl(self):
        file_data = ""
        if(self.component.GetType() == ComponentType.Client):
            file_data += """
            
"""
        elif(self.component.GetType() == ComponentType.Server):
            file_data += """
            
"""        

    def GetProtoDispatchDecl(self):
        file_data = ""
        if(self.component.GetType() == ComponentType.Client):
            file_data += """
    bool SendProtocol(Protocol& p)  {   return SendProtocol(&p);    }  
    bool SendProtocol(Protocol* p);
"""
        elif(self.component.GetType() == ComponentType.Server):
            file_data += """
    bool DispatchProtocol(Protocol& p)  {   return SendProtocol(&p);    }  
    bool SendProtocol(Protocol* p);              
"""        
        return file_data

    def GetProtoDispatchImpl(self):
        file_data = ""
        if(self.component.GetType() == ComponentType.Client):
            file_data += """
            
"""
        elif(self.component.GetType() == ComponentType.Server):
            file_data += """
            
"""        
        return file_data
    