

from ast import Str
import os
from module_data_parse import ModuleGen
from common import loginfo
from common import write_utf8
from module_file_gen import ModuleFileGen
from serialize_data_parse import SerializeDataType
from serialize_data_parse import SerializeData
from serialize_data_file_gen import SerializeDataFileGen
from proto_parse import ProtocolGen

class GenFile:
    def __init__(self, module_parse:ModuleGen, proto_parse:ProtocolGen, module_include_path:str, module_path:str, module_depoly_path:str) -> None:
        self.module_parse = module_parse
        self.proto_parse = proto_parse
        self.module_include_path = module_include_path
        self.module_path = module_path
        self.module_depoly_path = module_depoly_path
    def Gen(self):
        if(not self.GenSerailizeIncludeFile()):
            return False
        if(not self.GenModuleFile()):
            return False
        return True
    
    def GenModuleFile(self):
        for m in self.module_parse.modules.values():
            mp = ModuleFileGen(m, self.module_path, self.module_depoly_path)
            if(not mp.Gen()):
                loginfo("ModuleName=[{}] [{}]".format(m.name, "GenFileError"))
                return False
        return True

    def GenSerailizeIncludeFile(self):
        write_path = self.module_include_path
        if(not os.access(write_path, os.F_OK)):
            os.makedirs(write_path)
        for p in self.proto_parse.serialize_datas:
            fp = SerializeDataFileGen(p)
            data_file = fp.GenDataFile()

            if(p.GetType() == SerializeDataType.PROTOCOL):
                pass
            elif(p.GetType() == SerializeDataType.RPC):
                pass
            elif(p.GetType() == SerializeDataType.RPC_DATA):
                pass

            if(data_file and p.GetType() == SerializeDataType.RPC_DATA):
                extra = ""
                for other in self.proto_parse.serialize_datas:
                    if(other.GetType() != SerializeDataType.RPC_DATA or other.GetName() == p.GetName()):
                        continue
                    for member in p.members:
                        if(other.GetName() in member.GetType()):
                            extra += '#include "%s"\n' % other.GetDataName()
                            break
                if(extra):
                    data_file = data_file.replace('#include "oi_rpc.h"\n', '#include "oi_rpc.h"\n' + extra, 1)
            if(data_file):
                write_utf8(os.path.join(write_path, p.GetDataName()), data_file)
        if(not self.GenRpcIds(write_path)):
            return False
        return True

    def GenRpcIds(self, write_path):
        lines = []
        lines.append("#pragma once")
        lines.append("#include \"oi_header.h\"")
        lines.append("namespace OI { namespace Rpc {")
        lines.append("template<typename T> struct RpcProtoId;")
        lines.append("}}")
        lines.append("namespace OI { namespace RpcData {")
        for p in self.proto_parse.serialize_datas:
            if(p.GetType() != SerializeDataType.RPC):
                continue
            lines.append("class %s;" % p.GetReq())
            lines.append("class %s;" % p.GetRes())
        lines.append("}}")
        lines.append("namespace OI { namespace Rpc {")
        for p in self.proto_parse.serialize_datas:
            if(p.GetType() != SerializeDataType.RPC):
                continue
            lines.append("template<> struct RpcProtoId<::OI::RpcData::%s> { static const proto_type_t value = %s; };" % (p.GetReq(), p.GetID()))
            lines.append("template<> struct RpcProtoId<::OI::RpcData::%s> { static const proto_type_t value = %s; };" % (p.GetRes(), p.GetResID()))
        lines.append("}}")
        lines.append("")
        write_utf8(os.path.join(write_path, "rpc_ids.h"), "\n".join(lines) + "\n")
        return True