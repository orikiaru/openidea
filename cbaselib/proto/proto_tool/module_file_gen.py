from pathlib import Path
import re
import os
import shutil
from component_file_gen import ComponentFileGen
from common import loginfo
from common import write_utf8
from module_data_parse import Component, Module

class ModuleFileGen:
    def __init__(self, module: Module, module_root_path:str, module_depoly_path:str) -> None:
        self.module = module
        self.module_root_path = module_root_path
        self.module_path = ""
        self.module_deploy_path = module_depoly_path
        
        self.module_relative_config_path = "../config"
        
        self.common_header = '''#include <stdio.h>
#include "oi_net.h"
#include "oi_base.h"
'''
    
    def GetModulePath(self, short_path = False):
        if(not self.module_path):
            self.module_path = os.path.join(self.module_root_path, self.GetModuleDirName())
        if(short_path):
            return ""
        else:
            return self.module_path
    def GetConfigPath(self):
        return os.path.join(self.module_deploy_path, "config") + os.sep
    def GetModuleSrcPath(self, short_path = False):
        return os.path.join(self.GetModulePath(short_path), "src") + os.sep
    def GetModuleIncPath(self, short_path = False):
        return os.path.join(self.GetModulePath(short_path), "inc") + os.sep
    def GetModuleCMakePath(self, short_path = False):
        return os.path.join(self.GetModulePath(short_path), "CMakeLists.txt")
    def GetModuleMainPath(self, short_path = False):
        return os.path.join(self.GetModuleSrcPath(short_path), "main.cpp")
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
        file_data = file_data.replace("__MODULE_CONFIG_RELATIVE_PATH__", self.GetModuleConfigRelativePath())
        file_data = file_data.replace("__MODULE_NAME_LOWER__", self.GetModuleNameLower())
        file_data = file_data.replace("__CMAKE_MAIN_CPP_PATH__", "src/main.cpp")
        return file_data.replace("__MODULE_COMPONENTS_CONFIG__", self.GetComponentConfig())

    def Gen(self):
        #没有目录建一个
        if(not os.access(self.GetModulePath(), os.F_OK)):
            os.makedirs(self.GetModulePath())
        if(not os.access(self.GetModuleIncPath(), os.F_OK)):
            os.makedirs(self.GetModuleIncPath())
        if(not os.access(self.GetModuleSrcPath(), os.F_OK)):
            os.makedirs(self.GetModuleSrcPath())
        proto_path = os.path.join(self.GetModulePath(), "protocol")
        if os.path.isdir(proto_path):
            shutil.rmtree(proto_path)
        if(not os.access(self.GetConfigPath(), os.F_OK)):
            os.makedirs(self.DoReplace(self.GetConfigPath()))

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
        file_data += '#include "oi_module_components.h"\n'
        file_data += r'''
int main(int argc, char* argv[])
{
    //加载模块配置，初始化线程池、定时器、IO复用
    int32_t iCode = ::OI::Base::StartFramework("__MODULE_CONFIG_RELATIVE_PATH__", argc, argv);
    if (iCode != OI_OK)
    {
        fprintf(stderr, "StartFramework failed: code=%d\n", iCode);
        return iCode;
    }

    return ::OI::Base::RunFramework([]() -> int32_t
    {
        int32_t iCode = ::OI::Log::Init();
        if (iCode != OI_OK)
        {
            fprintf(stderr, "Log Init failed: code=%d\n", iCode);
            return iCode;
        }
        return ModuleComponents::Start();
    });
}
        '''
        file_data = self.DoReplace(file_data)
        
        includes = "".join(ComponentFileGen(c, self.module).GetIncludeFileData()
                           for c in self.module.components.values())
        startup = "".join("    " + line.rstrip() + "\n" if line.strip() else "\n"
                          for line in self.GetComponentInits().splitlines())
        declaration = "        int32_t iCode;\n" if startup else ""
        header = ('// Generated from module.proto.\n#pragma once\n#include "oi_base.h"\n#include <cstdio>\n'
                  + includes + '\nclass ModuleComponents\n{\n  public:\n    static int32_t Start()\n    {\n'
                  + declaration + startup + '        return OI_OK;\n    }\n};\n')
        self.WriteGenerated(Path(self.GetModuleIncPath()) / "oi_module_components.h", header)
        return self.InstallEntry(file_path, file_data, "ModuleComponents::Start()", self.MigrateMain)

    def WriteGenerated(self, path, content):
        if not path.exists() or path.read_text(encoding="utf-8") != content:
            write_utf8(path, content)

    def InstallEntry(self, path, template, hook, migrate):
        path = Path(path)
        if not path.exists():
            write_utf8(path, template)
            return True
        original = path.read_text(encoding="utf-8")
        if hook in original:
            return True
        updated = migrate(original, template)
        if updated is not None:
            write_utf8(path, updated)
            return True
        if self.module.components:
            loginfo(f"{path}: custom startup/build file. Replace the manual component list with {hook} "
                    "(include oi_module_components.h for C++), then export again.")
            return False
        return True

    def MigrateMain(self, original, template):
        # Only replace a main that matches the old generated skeleton in full.
        names = re.findall(r"^        iCode = (\w+)::GetInstance\(\)\.Init\(\);$", original, re.M)
        includes = ""
        startup = ""
        for name in names:
            component = Component()
            component.name = name
            component.type = "Server" if f"{name}::GetInstance().StartServer();" in original else "Client"
            gen = ComponentFileGen(component, self.module)
            includes += gen.GetIncludeFileData()
            startup += gen.GetInitString()
        old_startup = "".join("        " + line[4:] if line.strip() else line
                              for line in startup.splitlines(keepends=True))
        old = template.replace('#include "oi_module_components.h"\n', includes)
        old = old.replace("        return ModuleComponents::Start();", old_startup + "        return OI_OK;")
        return template if old == original else None

    def WriteFile(self, file_data:str, file_path:str, overwrite=False):
        #如果不复写,存在就退出,默认不复写
        if(not overwrite and os.access(file_path, os.F_OK)):
            loginfo("Module=[{}] file=[{}] already Gen".format(self.module.name, file_path))
            return True
        write_utf8(file_path, file_data)
        return True
    
    def GenComponentFile(self):
        marker = "// New protocol handler declarations are inserted here."
        for component in self.module.components.values():
            gen = ComponentFileGen(component, self.module)
            header_path = Path(self.GetModuleIncPath()) / gen.GetComponentHeaderFileName()
            source_path = Path(self.GetModuleSrcPath()) / gen.GetComponentCppFileName()
            old_header = gen.GenComponentHeader()
            fresh_header = old_header.replace("  public:\n", "  public:\n\tvoid RegisterProtocols();\n", 1)
            fresh_header = fresh_header.replace("};\n", f"  private:\n\t{marker}\n}};\n")
            if not header_path.exists() or header_path.read_text(encoding="utf-8") == old_header:
                write_utf8(header_path, fresh_header)
            self.WriteFile(gen.GenComponentCpp(), source_path)
            if not component.protos:
                old_binding = Path(self.GetModulePath()) / "generated" / f"{component.name.lower()}_protocols.cpp"
                if old_binding.exists():
                    self.WriteGenerated(old_binding, "// No protocol handlers declared in module.proto.\n")
                continue
            if not self.GenProtocolHandlers(component, header_path, source_path, marker):
                return False
        return True

    def GenProtocolHandlers(self, component, header_path, source_path, marker):
        header = header_path.read_text(encoding="utf-8")
        if not re.search(r"\bvoid\s+RegisterProtocols\s*\(\s*\)\s*;", header):
            loginfo(f"{header_path}: add public 'void RegisterProtocols();' and remove manual registration of module.proto handlers")
            return False
        binding = ["// Generated from module.proto. Business handlers belong in the component source.",
                   f'#include "{header_path.name}"', '#include "rpc_ids.h"']
        calls = []
        declarations = []
        source = source_path.read_text(encoding="utf-8")
        bodies = []
        request_headers = []
        for name, handler in component.protos.items():
            request = handler.protocol.GetReq()
            include = f'#include "oi_rpcdata_{request.lower()}.h"'
            binding.append(include)
            if handler.binder:
                if not re.search(rf"\b{handler.binder}\s*\(", header):
                    loginfo(f"{header_path}: declare and implement the custom binder {handler.binder}() for {name}")
                    return False
                calls.append(f"\t{handler.binder}();")
                continue
            method = "Handle" + name
            notify = handler.kind == "notify"
            api = "RegisterNotifyHandler" if notify else "RegisterRpcHandler"
            trait = "RpcNotifyTypes" if notify else "RpcHandlerTypes"
            field = "Message" if notify else "Request"
            member = f"&{component.name}::{method}"
            calls.extend([f"\tusing {name}Args = ::OI::Rpc::RpcHandlerArgs<decltype({member})>::Args;",
                          f"\tstatic_assert(std::is_same_v<::OI::Rpc::{trait}<{name}Args>::{field}, ::OI::RpcData::{request}>);",
                          f"\t{api}({member});"])
            if re.search(rf"\b{method}\s*\(", header):
                continue
            if marker not in header:
                loginfo(f"{header_path}: implement {method}, or add '{marker}' inside the class to scaffold new handlers")
                return False
            params = f"const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::{request}& stRequest"
            if not notify:
                params += f", ::OI::RpcData::{handler.protocol.GetRes()}& stResponse"
            declarations.append(f"\t::OI::Rpc::RpcTask {method}({params});\n")
            if re.search(r"\b" + re.escape(component.name + "::" + method) + r"\s*\(", source):
                continue
            request_headers.append(include)
            bodies.extend(["", f"::OI::Rpc::RpcTask {component.name}::{method}({params})", "{",
                           "\t(void)stContext;", "\t(void)stRequest;"])
            if not notify:
                bodies.append("\t(void)stResponse;")
            bodies.extend(["\tco_return OI_ERR_FAIL; // Implement the business handler.", "}"])
        if declarations:
            anchor = re.search(r"(?m)^[ \t]*" + re.escape(marker) + r"[ \t]*$", header)
            if not anchor:
                loginfo(f"{header_path}: the handler insertion marker must be on its own line")
                return False
            header = header[:anchor.start()] + "".join(declarations) + header[anchor.start():]
            if '#include "rpc_ids.h"' not in header:
                header = header.replace("#pragma once", '#pragma once\n#include "rpc_ids.h"', 1)
            if bodies:
                includes = [line for line in request_headers if line not in source]
                self.WriteGenerated(source_path, "\n".join(includes + [""]) + source.rstrip() + "\n" + "\n".join(bodies) + "\n")
            self.WriteGenerated(header_path, header)
        binding.extend(["", f"void {component.name}::RegisterProtocols()", "{", *calls, "}", ""])
        generated = Path(self.GetModulePath()) / "generated"
        generated.mkdir(exist_ok=True)
        self.WriteGenerated(generated / f"{component.name.lower()}_protocols.cpp", "\n".join(binding))
        return True

    def GenCMakeFile(self):
        file_path = self.GetModuleCMakePath()
        file_data = '''
cmake_minimum_required(VERSION 2.6)

###### __MODULE_NAME_LOWER__ CMake文件 ######

#设置可执行程序依赖的源文件
set(MODULE_SRCS 
        __CMAKE_MAIN_CPP_PATH__
    )
#可执行程序
add_executable(__MODULE_NAME_LOWER__ ${MODULE_SRCS})
include(oi_module_components.cmake)

#头文件目录
target_include_directories(__MODULE_NAME_LOWER__
    PUBLIC
		inc
)

set(SINGLE_THREAD TRUE)

#链接base,net,proto,timer,logger
if(MSVC)
	if(SINGLE_THREAD)
		target_link_libraries(__MODULE_NAME_LOWER__ PUBLIC ${BASE_LIBS})
		target_compile_definitions(__MODULE_NAME_LOWER__ PUBLIC _SINGLE_THREAD_)
	else()
		target_link_libraries(__MODULE_NAME_LOWER__ PUBLIC ${BASE_M_LIBS})
	endif()
else()
	if(SINGLE_THREAD)
		target_link_libraries(__MODULE_NAME_LOWER__ PUBLIC -Wl,--start-group ${BASE_LIBS} -Wl,--end-group)
		target_compile_definitions(__MODULE_NAME_LOWER__ PUBLIC _SINGLE_THREAD_)
	else()
		target_link_libraries(__MODULE_NAME_LOWER__ PUBLIC -Wl,--start-group ${BASE_M_LIBS} -Wl,--end-group)
		target_link_libraries(__MODULE_NAME_LOWER__ PUBLIC pthread)
	endif()
endif()


'''
        file_data = self.DoReplace(file_data)
        sources = [f"    src/{c.name.lower()}.cpp" for c in self.module.components.values()]
        sources.extend(f"    generated/{c.name.lower()}_protocols.cpp" for c in self.module.components.values() if c.protos)
        fragment = "# Generated from module.proto.\n"
        if sources:
            fragment += f"target_sources({self.GetModuleNameLower()} PRIVATE\n" + "\n".join(sources) + "\n)\n"
        self.WriteGenerated(Path(self.GetModulePath()) / "oi_module_components.cmake", fragment)
        return self.InstallEntry(file_path, file_data, "include(oi_module_components.cmake)", self.MigrateCMake)

    def MigrateCMake(self, original, template):
        sources = re.search(r"^        src/main\.cpp (?:src/\w+\.cpp )*$", original, re.M)
        if not sources:
            return None
        old = template.replace("\ninclude(oi_module_components.cmake)", "")
        old = old.replace("        src/main.cpp\n", sources.group(0) + "\n")
        return template if old == original else None

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
threadcount = 3 #协程工人线程数
'''
        file_data = self.DoReplace(file_data)
        path = Path(file_path)
        if not path.exists():
            write_utf8(path, file_data)
            return True
        existing = path.read_text(encoding="utf-8")
        missing = []
        for component in self.module.components.values():
            name = re.escape(component.name)
            section = rf"""^\s*\[\s*(?:{name}|"{name}"|'{name}')\s*\]\s*(?:#.*)?$"""
            if not re.search(section, existing, re.M):
                missing.append(ComponentFileGen(component, self.module).GetConfigString())
        if missing:
            with path.open("a", encoding="utf-8", newline="\n") as output:
                output.write("\n" + "".join(missing))
        return True
