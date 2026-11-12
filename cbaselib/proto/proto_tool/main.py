import os
import sys

from common import loginfo
from module_data_parse import ModuleGen
from gen_file import GenFile
from proto_parse import ProtocolGen
# coding=<utf-8>
work_path = os.getcwd()
#本地IP 通过getlocalip()获取
local_ip = ''   

def SplitArgs(argv):
    if "--" not in argv:
        return None
    cut = argv.index("--")
    return argv[1:cut], argv[cut + 1:]

def GenDataOnly(proto_files, output):
    if not proto_files:
        loginfo("need at least one proto file")
        return False
    proto = ProtocolGen(proto_files)
    if not proto.Gen():
        return False
    file_gen = GenFile(None, proto, output, '', '')
    return file_gen.GenSerailizeIncludeFile()

def RunModules(proto_files, module_file, component_file, module_header_path, module_path, module_depoly_path, module_manifest_path):
    proto = ProtocolGen(proto_files)
    if not proto.Gen():
        return -1
    module = ModuleGen(proto, module_file, component_file)
    if not module.Gen():
        return -2
    file_gen = GenFile(module, proto, module_header_path + '/', module_path + '/', module_depoly_path + '/')
    if not file_gen.Gen(module_manifest_path):
        return -3
    return 0

# 协议文件放在 -- 后面，可以有多份。-- 前面是 module、component、include、module 目录、deploy，manifest 可选。
# 旧写法仍可用：第一个位置参数是唯一的协议文件。--data-only 同样两种都认。
if __name__ == "__main__":
    loginfo("local path = ", work_path)

    split = SplitArgs(sys.argv)
    if split is not None:
        head, proto_files = split
        if head[:1] == ["--data-only"]:
            if len(head) != 2:
                loginfo("data-only needs output directory then -- proto files")
                sys.exit(1)
            sys.exit(0 if GenDataOnly(proto_files, head[1]) else 1)
        if len(head) not in (5, 6) or not proto_files:
            loginfo("need module, component, include, module dir, deploy, optional manifest, --, proto files")
            sys.exit(1)
        manifest = head[5] if len(head) == 6 else None
        sys.exit(RunModules(proto_files, head[0], head[1], head[2], head[3], head[4], manifest))

    # 基础库内部协议只生成现有 rpcdata/枚举，不生成应用模块。
    if len(sys.argv) == 4 and sys.argv[1] == '--data-only':
        sys.exit(0 if GenDataOnly([sys.argv[2]], sys.argv[3]) else 1)

    command_params = []
    #命令行参数要有 协议目录,协议生成根目录,协议公用序列化接口目录
    if(len(sys.argv) < 7):
        loginfo("comand_param_count= less than 6")
        loginfo("we need proto_file_path")
        loginfo("we need module_path")
        loginfo("we need component_path")
        loginfo("we need module_header_path")
        loginfo("we need module_path")
        loginfo("we need deploy_path")
        
        if(len(sys.argv) == 1 or sys.argv[1] == 'test'):
            work_path += "/cbaselib/proto"
            command_params.append(work_path+"/config/proto.xml")
            command_params.append(work_path+"/config/module.xml")
            command_params.append(work_path+"/config/module_component.xml")
            command_params.append(work_path+"/module_gen_test/include/proto_header")
            command_params.append(work_path+'/module_gen_test')
            command_params.append(work_path+'/module_gen_test/deploy')
        exit(-2)
    else:
        for index in range(1,7):
            command_params.append(sys.argv[index])
            
    proto_xml_file_path = command_params[0]         #协议文件路径
    module_file         = command_params[1]         #模块文件路径
    component_file      = command_params[2]         #组件文件路径
    module_header_path  = command_params[3]         #协议公用序列化逻辑导出路径 [rpcdata  和 rpc protocol的序列化逻辑 ]
    module_path         = command_params[4]         #module的生成目录,包括module基础代码,module的组件基础代码,module组件涉及的协议的处理代码
    module_depoly_path  = command_params[5]         #程序部署路径
    module_manifest_path = sys.argv[7] if len(sys.argv) > 7 else None
    exit(RunModules([proto_xml_file_path], module_file, component_file, module_header_path, module_path, module_depoly_path, module_manifest_path))
