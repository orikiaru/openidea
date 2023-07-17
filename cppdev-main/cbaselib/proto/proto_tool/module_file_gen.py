from encodings import utf_8
import os
from component_file_gen import ComponentFileGen
from common import loginfo
from module_data_parse import Module
from serialize_data_file_gen import SerializeDataFileGen
from serialize_data_parse import SerializeDataType

class ModuleFileGen:
    def __init__(self, module: Module, module_root_path:str, module_depoly_path:str) -> None:
        self.module = module
        self.module_root_path = module_root_path
        self.module_path = ""
        self.module_deploy_path = module_depoly_path
        
        self.module_relative_config_path = "../config"
        
        self.common_header = '''#include <stdio.h>
#include "net_header.h"
#include "base_header.h"
'''
    
    def GetModulePath(self, short_path = False):
        if(not self.module_path):
            self.module_path = self.module_root_path + self.GetModuleDirName()
        if(short_path):
            return ""
        else:
            return self.module_path
    def GetConfigPath(self):
        return self.module_deploy_path + "/config/"
    def GetModuleSrcPath(self, short_path = False):
        return self.GetModulePath(short_path) + "src/"
    def GetModuleIncPath(self, short_path = False):
        return self.GetModulePath(short_path) + "inc/"
    def GetModuleProtoPath(self, short_path = False):
        return self.GetModulePath(short_path) + "protocol/"
    def GetModuleStubPath(self, short_path = False):
        return self.GetModuleSrcPath(short_path) + "proto_stub.cpp"
    def GetModuleCMakePath(self, short_path = False):
        return self.GetModulePath(short_path) + "CMakeLists.txt"
    def GetModuleMainPath(self, short_path = False):
        return self.GetModuleSrcPath(short_path) + "main.cpp"
    def GetModuleConfigName(self):
        return "__MODULE_NAME_LOWER__" + ".toml"
    def GetModuleConfigPath(self):
        return self.GetConfigPath() + self.GetModuleConfigName()
    def GetModuleConfigRelativePath(self):
        return self.GetConfigRelativePath() + "/" + self.GetModuleConfigName()
    
    #提供给DoReplace外的接口
    def GetRealModuleConfigPath(self):
        str = self.GetModuleConfigPath()
        return self.DoReplace(str)
    
    def GetCompontCppsPath(self, short_path = False):
        paths = ""  
        for c in self.module.components.values():
            cg = ComponentFileGen(c, self.module)
            paths += self.GetModuleSrcPath(short_path) + cg.GetComponentCppFileName() + " "
        return paths
    
    def GetModuleDirName(self):
        return self.module.name.lower() + "/"

    def GetModuleNameLower(self):
        return self.module.name.lower()
    
    def GetModuleName(self):
        return self.module.name
    
    def GetComponentInits(self) -> str:
        file_data = ""
        for c in self.module.components.values():
            cg = ComponentFileGen(c, self.module) 
            file_data += cg.GetInitString()
        return file_data
    
    def GetComponentConfig(self) ->str:
        file_data = ""
        for c in self.module.components.values():
            cg = ComponentFileGen(c, self.module) 
            file_data += cg.GetConfigString()
        return file_data
    
    def GetConfigRelativePath(self)->str:
        return self.module_relative_config_path
    
    def DoReplace(self, file_data:str):
        #module_name = self.GetModuleName()
        module_name_lower = self.GetModuleNameLower()

        #stub_file_path = self.GetModuleStubPath()
        cmake_stub_file_path = self.GetModuleStubPath(True)

        #component_cpp_path = self.GetCompontCppsPath()
        cmake_component_cpp_path = self.GetCompontCppsPath(True)

        #main_cpp_path = self.GetModuleMainPath()
        cmake_main_cpp_path = self.GetModuleMainPath(True)
        
        module_config_path = self.GetModuleConfigPath()
        
        module_components_init = self.GetComponentInits()
        
        module_components_config = self.GetComponentConfig()
        
        module_config_relative_path = self.GetModuleConfigRelativePath()

        #里面依赖__MODULE_NAME_LOWER__,所以需要在前面
        file_data = file_data.replace("__MODULE_CONFIG_PATH__", module_config_path)
        file_data = file_data.replace("__MODULE_CONFIG_RELATIVE_PATH__", module_config_relative_path)
        
        file_data = file_data.replace("__MODULE_NAME_LOWER__", module_name_lower)
        file_data = file_data.replace("__CMAKE_STUB_FILE_PATH__",cmake_stub_file_path)
        file_data = file_data.replace("__CMAKE_COMPONTENTS_CPP_PATH__",cmake_component_cpp_path)
        file_data = file_data.replace("__CMAKE_MAIN_CPP_PATH__",cmake_main_cpp_path)
        file_data = file_data.replace("__MODULE_COMPONENTS_INIT__", module_components_init)
        file_data = file_data.replace("__MODULE_COMPONENTS_CONFIG__", module_components_config)
        
        return file_data
        
    def Gen(self):
        #没有目录建一个
        if(not os.access(self.GetModulePath(), os.F_OK)):
            os.makedirs(self.GetModulePath())
        if(not os.access(self.GetModuleIncPath(), os.F_OK)):
            os.makedirs(self.GetModuleIncPath())
        if(not os.access(self.GetModuleSrcPath(), os.F_OK)):
            os.makedirs(self.GetModuleSrcPath())
        if(not os.access(self.GetModuleProtoPath(), os.F_OK)):
            os.makedirs(self.GetModuleProtoPath())
        if(not os.access(self.GetConfigPath(), os.F_OK)):
            os.makedirs(self.DoReplace(self.GetConfigPath()))
            
        if(not self.GenProtocolAndStub()):
            loginfo("ModuleName=[{}] [{}]".format(self.module.name, "GenProtocolAndStubError"))
            return False
        if(not self.GenModuleFile()):
            loginfo("ModuleName=[{}] [{}]".format(self.module.name, "GenModuleFileError"))
            return False
        if(not self.GenComponentFile()):
            loginfo("ModuleName=[{}] [{}]".format(self.module.name, "GenComponentFileError"))
            return False
        if(not self.GenCMakeFile()):
            loginfo("ModuleName=[{}] [{}]".format(self.module.name, "GenCMakeFileError"))
            return False
        if(not self.GenModuleConfig()):
            loginfo("ModuleName=[{}] [{}]".format(self.module.name, "GenModuleConfigError"))
            return False
        return True
    
    def GenModuleFile(self):
        #生成模块main函数
        file_path = self.GetModuleSrcPath() + "/main.cpp"
        
        file_data = ""
        file_data += self.common_header
        for c in self.module.components.values():
            #这部分交给模块自己生成
            cg = ComponentFileGen(c,self.module)
            file_data += cg.GetIncludeFileData()
        file_data += r'''
int main(int argc, char* argv[])
{
    //加载模块配置
    if (!Config::GlobalMutableConfig().Parse("__MODULE_CONFIG_RELATIVE_PATH__"))
	{
		printf("ParseConfigError\n");
		return -2;
	}
 
    //初始化线程池
    ThreadPool::GetInstance()->Init();
    
    //初始化定时器
    BaseTimerManager::GetInstance()->Init();
    
    //初始化IO复用
	Poll::GetInstance()->Init();

    //初始化日志服务
    Log::Init();
 
    //添加组件逻辑,如网络组件,自定义单例对象等初始化逻辑
    __MODULE_COMPONENTS_INIT__
 
    //启动线程池,驱动进程逻辑
    ThreadPool::GetInstance()->StartWork();
    
    //此处是走不到的.
    return 0;
}
        '''
        file_data = self.DoReplace(file_data)
        
        self.WriteFile(file_data, file_path, False)
        return True
    
    def WriteFile(self, file_data:str, file_path:str, overwrite=False):
        #如果不复写,存在就退出,默认不复写
        if(not overwrite and os.access(file_path, os.F_OK)):
            loginfo("Module=[{}] file=[{}] already Gen".format(self.module.name, file_path))
            return True
        fp = open(file_path, "w+")
        if(not fp):
            loginfo("Module=[{}] header open_file_error path={}".format(self.module.name, file_path))
            return False
        fp.write(file_data)
        fp.close()
        return True
    
    def GenComponentFile(self):
        for c in self.module.components.values():
            cf = ComponentFileGen(c, self.module)
            
            file_data = cf.GenComponentHeader()
            self.WriteFile(file_data, self.GetModuleIncPath() + cf.GetComponentHeaderFileName())
            
            file_data = cf.GenComponentCpp()
            self.WriteFile(file_data, self.GetModuleSrcPath() + cf.GetComponentCppFileName())

        return True
    
    def GenProtocolAndStub(self):
        
        #GenStub 协议存根暂时生成在src目录
        file_path = self.GetModuleStubPath()
        #先写所有依赖头文件
        file_data = '#include "header.h" \n'
        for p in self.module.protos.values():
            if(p.GetType() != SerializeDataType.PROTOCOL and p.GetType() != SerializeDataType.RPC):
                continue
            file_data += '#include "' + p.GetProtoFileName() + '" \n'
        
        #后写所有存根注册
        file_data += '''
void s_RegisterStub() FUNCTION_RUN_BEFORE_MAIN;
void s_RegisterStub()
{
'''
        
        for p in self.module.protos.values():
            #这部分交给SerializeDataFileGen替换,关键词均与协议有关,而且高度重合
            s_gen = SerializeDataFileGen(p)
            file_data += "\t" + s_gen.GenStubFile()
            
        file_data += '''
}
''' 
        
        self.WriteFile(file_data, file_path, True) #存根每次均重新生成
        
        
        #GenProtocol
        proto_root_gen_path = self.GetModuleProtoPath()
        if(not os.access(proto_root_gen_path, os.F_OK)):
            os.makedirs(proto_root_gen_path)
        proto_root_gen_path += "/"
        
        for c in self.module.protos.values():
            gen_path =  proto_root_gen_path + c.GetProtoFileName()
            f = SerializeDataFileGen(c)
            file_data = f.GenHeaderFile()
            self.WriteFile(file_data, gen_path, False) #模块的协议文件存在就不重新生成.
        return True
            
    def GenCMakeFile(self):
        file_path = self.GetModuleCMakePath()
        file_data = '''
cmake_minimum_required(VERSION 2.6)

###### __MODULE_NAME_LOWER__ CMake文件 ######

#设置可执行程序依赖的源文件
set(MODULE_SRCS 
        __CMAKE_STUB_FILE_PATH__ __CMAKE_MAIN_CPP_PATH__ __CMAKE_COMPONTENTS_CPP_PATH__
    )
#可执行程序
add_executable(__MODULE_NAME_LOWER__ ${MODULE_SRCS})

#头文件目录
target_include_directories(__MODULE_NAME_LOWER__
    PUBLIC
		inc
		protocol
)

set(SINGLE_THREAD TRUE)

#链接base,net,proto,timer,logger
if(SINGLE_THREAD)
	target_link_libraries(__MODULE_NAME_LOWER__ PUBLIC -Wl,--start-group ${BASE_LIBS} -Wl,--end-group)
else()
	target_link_libraries(__MODULE_NAME_LOWER__ PUBLIC -Wl,--start-group ${BASE_M_LIBS} -Wl,--end-group)
endif()

if(SINGLE_THREAD)
	#__MODULE_NAME_LOWER__为单线程,需要设置_SINGLE_THREAD_宏
	target_compile_definitions(__MODULE_NAME_LOWER__ PUBLIC _SINGLE_THREAD_)
else()
	#__MODULE_NAME_LOWER__为多线程,需要链接pthread
	target_link_libraries(__MODULE_NAME_LOWER__ PUBLIC pthread)
endif()


'''
        file_data = self.DoReplace(file_data)
        self.WriteFile(file_data, file_path, False) #不覆写.可能会盖掉自定义编译内容和新加的tag
        return True
    def GenModuleConfig(self):
        file_path = self.GetRealModuleConfigPath()
        file_data = '''# __MODULE_NAME_LOWER__ config
[Service]
name = "__MODULE_NAME_LOWER__"
id = 1

__MODULE_COMPONENTS_CONFIG__

[LogClient]
ip		= "127.0.0.1"
port	= "19995"
type   	= "tcp"

[Timer]
offset_ms = 50		#定时器间隔,必须被1000整除
timer_lv_count = 4	#定时器分割等级数,必须>=2
#每一个偏移等级对应的tick数基数
#lv[0] = timer_lvs[0]
#lv[1] = timer_lvs[0]*timer_lvs[1]
#lv[2] = timer_lvs[0]*timer_lvs[1]*timer_lvs[2]
#lv[3] = timer_lvs[0]*timer_lvs[1]*timer_lvs[2]*timer_lvs[3]
timer_lvs = [128,128,128,1] #每级定时器对应的间隔,最后一级只能是1

[ThreadPool]
normal = 3 #标准线程池数
urgent = 1 #加急线程池数
queue = 1  #有序线程池数
max_task_count = 100000
'''
        file_data = self.DoReplace(file_data)
        self.WriteFile(file_data, file_path, False) #不覆写.可能会盖掉自定义编译内容和新加的tag
        return True