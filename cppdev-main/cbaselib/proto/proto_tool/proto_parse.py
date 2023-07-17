#协议生成辅助工具
#协议结构暂时使用xml

#要求python版本3.x

from typing import List,Tuple,Dict,Union,Optional
from common import openxml
from common import loginfo
from common import openxml
from serialize_data_parse import SerializeData, SerializeDataType



class ProtocolGen:
    def __init__(self, xml_path):
        self.proto_id_map = {}      #type:Dict[str,SerializeData] 
        self.type_name_map = {}     #type:Dict[str,SerializeData]
        self.xml_path = xml_path
        self.serialize_datas = []   #type:List[SerializeData]
        self.GenDefaultProtocol()
    
    def GenDefaultProtocol(self):
        return
    
    def GetProtocol(self, proto_name:str):
        if(proto_name in self.type_name_map and self.type_name_map[proto_name].IsProtocol()):
            return self.type_name_map[proto_name]
        return None
        
    def Gen(self)->bool:
        path=self.xml_path
        tree=openxml(path)
        if tree is None:
            loginfo("ServerConfigReplace::DoReplace error can't open xml = ", path)
            exit(-2)
        root=tree.getroot()

        #序列化数据结构生成
        ret = self.GenRpcData(root)
        if(not ret):
            return False
        
        #RPC生成
        ret = self.GenRpc(root)
        if(not ret):
            return False
        
        #协议文件生成
        ret = self.GenProtocols(root)
        if(not ret):
            return False
        
        return True
    
    def GenRpc(self,root)->bool:
        rpcs=root.findall("rpc")
        for rpc in rpcs:
            p = SerializeData(SerializeDataType.RPC, rpc)
            if(not p.Parse()):
                return False
            if(not self.CheckSerializeData(SerializeDataType.RPC, p)):
                return False
            self.serialize_datas.append(p)
        return True
        
    def GenRpcData(self,root)->bool:
        rpc_datas=root.findall("rpcdata")
        for rpc_data in rpc_datas:
            p = SerializeData(SerializeDataType.RPC_DATA, rpc_data)
            if(not p.Parse()):
                return False
            if(not self.CheckSerializeData(SerializeDataType.RPC_DATA, p)):
                return False
            self.serialize_datas.append(p)
        return True
    
    def GenProtocols(self,root)->bool:
        protos=root.findall("protocol")
        for proto in protos:
            p = SerializeData(SerializeDataType.PROTOCOL, proto)
            if(not p.Parse()):
                return False
            if(not self.CheckSerializeData(SerializeDataType.PROTOCOL,p)):
                return False
            self.serialize_datas.append(p)
        return True
 
    def CheckSerializeData(self, serialize_type, data)->bool:
        #排除同名,所有生成类不可重名
        if(data.GetName() in self.type_name_map):
            loginfo("CheckRpcData name repeat old_name=", data.GetName())
            return False
        
        if(serialize_type == SerializeDataType.PROTOCOL or serialize_type == SerializeDataType.RPC):
            #排除同协议号
            if(data.GetID() in self.proto_id_map):
                loginfo("CheckProtocol id repeat old_name=", self.proto_id_map[data.GetID()].GetName(), ",new_name=", data.GetName())
                return False
            self.proto_id_map[data.GetID()] = data
            
        self.type_name_map[data.GetName()] = data
        return True

