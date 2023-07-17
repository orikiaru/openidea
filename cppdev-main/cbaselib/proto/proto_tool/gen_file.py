

from ast import Str
import os
from module_data_parse import ModuleGen
from common import loginfo
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

            if(data_file):
                fd= open(write_path + p.GetDataName(), "w")
                if(not fd):
                    loginfo("GenSerailizeIncludeFile open error 2")
                    return False
                fd.write(data_file)
                fd.close()    
        return True