#协议生成辅助工具
#协议结构暂时使用xml

#要求python版本3.x

import os
from typing import List,Tuple,Dict,Union,Optional
from common import openxml
from common import loginfo
from common import openxml
from serialize_data_parse import SerializeData, SerializeDataType
from proto_text import ParseProtoText
import re

# 这些标量自带 operator<，可以当 map 键，也可以放进 key 结构。
ORDERED_SCALARS = {
    "bool",
    "int8_t", "uint8_t", "int16_t", "uint16_t", "int32_t", "uint32_t", "int64_t", "uint64_t",
    "float", "double",
    "std::string",
}
# JSON 对象的键只能是字符串。整数键写成十进制字符串，和 oi_json.h 里已有的重载对齐。
JSON_MAP_KEYS = {"std::string", "int32_t", "uint32_t", "uint64_t"}
JSON_SCALARS = ORDERED_SCALARS | {"Bytes"}

def NormType(text):
    return re.sub(r"\s+", "", text or "")

def SplitTypeArgs(text):
    parts = []
    depth = 0
    cur = []
    for ch in text:
        if ch == "<":
            depth += 1
            cur.append(ch)
        elif ch == ">":
            depth -= 1
            cur.append(ch)
        elif ch == "," and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    if cur:
        parts.append("".join(cur))
    return parts

def ParseType(text):
    text = NormType(text)
    if text.startswith("std::vector<") and text.endswith(">"):
        return ("vector", ParseType(text[len("std::vector<"):-1]))
    if text.startswith("std::map<") and text.endswith(">"):
        args = SplitTypeArgs(text[len("std::map<"):-1])
        if len(args) != 2:
            return ("bad", text)
        return ("map", ParseType(args[0]), ParseType(args[1]))
    return ("name", text)

def IterMapKeys(parsed):
    if parsed[0] == "map":
        yield parsed[1]
        for key in IterMapKeys(parsed[1]):
            yield key
        for key in IterMapKeys(parsed[2]):
            yield key
    elif parsed[0] == "vector":
        for key in IterMapKeys(parsed[1]):
            yield key



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
        if not os.path.exists(path):
            loginfo("ERROR: not find:" + path)
            exit(-2)
        with open(path, "r", encoding="utf-8-sig") as fp:
            text = fp.read()
        if text.lstrip().startswith("<"):
            tree=openxml(path)
            if tree is None:
                exit(-2)
            root=tree.getroot()
            ret = self.GenRpcData(root)
            if(not ret):
                return False
            ret = self.GenRpc(root)
            if(not ret):
                return False
            return True
        loginfo("open proto file path:" + path)
        datas = ParseProtoText(text)
        if datas is None:
            return False
        for p in datas:
            if not self.CheckSerializeData(p.GetType(), p):
                return False
            self.serialize_datas.append(p)
        return self.CheckKeyComparators()
    
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
        return self.CheckKeyComparators()

    def RpcDataByName(self, name):
        data = self.type_name_map.get(name)
        if data is None or data.GetType() != SerializeDataType.RPC_DATA:
            return None
        return data

    # key 结构的每个成员都要能比较。嵌套的 rpcdata 必须自己也标了 key。
    def TypeOrdered(self, parsed, stack):
        if parsed[0] == "bad":
            return False
        if parsed[0] == "name":
            if parsed[1] in ORDERED_SCALARS:
                return True
            data = self.RpcDataByName(parsed[1])
            if data is None or not data.is_key:
                return False
            if parsed[1] in stack:
                return False
            stack.add(parsed[1])
            for member in data.members:
                if not self.TypeOrdered(ParseType(member.GetType()), stack):
                    stack.remove(parsed[1])
                    return False
            stack.remove(parsed[1])
            return True
        if parsed[0] == "vector":
            return self.TypeOrdered(parsed[1], stack)
        if parsed[0] == "map":
            return self.TypeOrdered(parsed[1], stack) and self.TypeOrdered(parsed[2], stack)
        return False

    def TypeJsonOk(self, parsed, stack):
        if parsed[0] == "bad":
            return False
        if parsed[0] == "name":
            if parsed[1] in JSON_SCALARS:
                return True
            data = self.RpcDataByName(parsed[1])
            if data is None or data.is_key or parsed[1] in stack:
                return False
            stack.add(parsed[1])
            for member in data.members:
                if not self.TypeJsonOk(ParseType(member.GetType()), stack):
                    stack.remove(parsed[1])
                    return False
            stack.remove(parsed[1])
            return True
        if parsed[0] == "vector":
            return self.TypeJsonOk(parsed[1], stack)
        if parsed[0] == "map":
            key = parsed[1]
            if key[0] != "name" or key[1] not in JSON_MAP_KEYS:
                return False
            return self.TypeJsonOk(parsed[2], stack)
        return False

    def CheckKeyComparators(self)->bool:
        for data in self.serialize_datas:
            if data.GetType() != SerializeDataType.RPC_DATA:
                continue
            if data.is_key and len(data.members) == 0:
                loginfo("key rpcdata need members name=", data.GetName())
                return False
            for member in data.members:
                parsed = ParseType(member.GetType())
                for key in IterMapKeys(parsed):
                    if not self.TypeOrdered(key, set()):
                        loginfo("map key has no comparator owner=", data.GetName(), ",field=", member.GetName(), ",key=", key)
                        return False
            if data.is_key:
                for member in data.members:
                    if not self.TypeOrdered(ParseType(member.GetType()), set([data.GetName()])):
                        loginfo("key rpcdata nested comparator missing name=", data.GetName(), ",field=", member.GetName(), ",type=", member.GetType())
                        return False
            data.json_ok = (not data.is_key) and all(self.TypeJsonOk(ParseType(member.GetType()), set()) for member in data.members)
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

