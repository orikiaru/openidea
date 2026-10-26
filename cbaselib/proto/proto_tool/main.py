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

#根据指定xml文件 生成协议,rpc,rpcdata
if __name__ == "__main__":
    loginfo("local path = ", work_path)

    command_params = []
    #命令行参数要有 协议目录,协议生成根目录,协议公用序列化接口目录
    if(len(sys.argv) < 7):
        loginfo("comand_param_count= less than 6")
        loginfo("we need proto_xml_file_path")
        loginfo("we need module_xml_path")
        loginfo("we need component_xml_path")
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
            
    proto_xml_file_path = command_params[0]         #协议xml文件路径
    module_xml_path     = command_params[1]         #模块xml文件路径
    component_xml_path  = command_params[2]         #组件xml文件路径
    module_header_path  = command_params[3]+'/'     #协议公用序列化逻辑导出路径 [rpcdata  和 rpc protocol的序列化逻辑 ]
    module_path         = command_params[4]+'/'     #module的生成目录,包括module基础代码,module的组件基础代码,module组件涉及的协议的处理代码
    module_depoly_path  = command_params[5]+'/'     #程序部署路径

    proto = ProtocolGen(proto_xml_file_path) 
    ret = proto.Gen()
    if(not ret):
        exit(-1)
    
    module = ModuleGen(proto, module_xml_path, component_xml_path)
    ret = module.Gen()
    if(not ret):
        exit(-2)
        
    file_gen = GenFile(module, proto, module_header_path, module_path, module_depoly_path)
    ret = file_gen.Gen()
    if(not ret):
        exit(-3)
        
    exit(0)