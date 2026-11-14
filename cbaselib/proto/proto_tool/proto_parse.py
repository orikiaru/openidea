#协议生成辅助工具
#协议结构暂时使用xml

#要求python版本3.x

import os
from typing import List,Tuple,Dict,Union,Optional
from common import openxml
from common import loginfo
from common import openxml
from serialize_data_parse import SerializeData, SerializeDataType
from serialize_data_parse import DB_BINARY_TYPES, DB_SIZED_TYPES, DB_TEXT_TYPES, DbTypeBase
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

DB_KEY_MAX_BYTES = 3072

# 主键列在索引里占的字节：utf8mb4 文本每字符 4 字节，二进制按长度，数值和 bool 按 8 字节上限算。
def DbKeyBytes(db_type):
    if not db_type:
        return 8
    match = re.fullmatch(r"([A-Z]+)\((\d+)\)", db_type)
    if not match:
        return 8
    length = int(match.group(2))
    return length * 4 if match.group(1) in ("VARCHAR", "CHAR") else length

# vector<bool> 是标准库特化，序列化层不支持。任何一层出现都在生成期拒绝。
def HasVectorBool(parsed):
    if parsed[0] == "vector":
        return parsed[1] == ("name", "bool") or HasVectorBool(parsed[1])
    if parsed[0] == "map":
        return HasVectorBool(parsed[1]) or HasVectorBool(parsed[2])
    return False

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
        if isinstance(xml_path, (str, os.PathLike)):
            self.xml_paths = [os.fspath(xml_path)]
        else:
            self.xml_paths = [os.fspath(path) for path in xml_path]
        self.xml_path = self.xml_paths[0] if self.xml_paths else ""
        self.serialize_datas = []   #type:List[SerializeData]
        self.GenDefaultProtocol()
    
    def GenDefaultProtocol(self):
        return
    
    def GetProtocol(self, proto_name:str):
        if(proto_name in self.type_name_map and self.type_name_map[proto_name].IsProtocol()):
            return self.type_name_map[proto_name]
        return None
        
    def Gen(self)->bool:
        if not self.xml_paths:
            loginfo("ERROR: no proto file")
            return False
        for path in self.xml_paths:
            if not self.LoadFile(path):
                return False
        return self.CheckEnums() and self.CheckRpcPayloads() and self.CheckKeyComparators()

    def LoadFile(self, path)->bool:
        if not os.path.exists(path):
            loginfo("ERROR: not find:" + path)
            exit(-2)
        loginfo("open proto file path:" + path)
        with open(path, "r", encoding="utf-8-sig") as fp:
            text = fp.read()
        if text.lstrip().startswith("<"):
            tree=openxml(path)
            if tree is None:
                exit(-2)
            root=tree.getroot()
            if not self.GenRpcData(root) or not self.GenRpc(root):
                return False
            return True
        datas = ParseProtoText(text)
        if datas is None:
            loginfo("proto file failed path:" + path)
            return False
        for p in datas:
            if not self.CheckSerializeData(p.GetType(), p):
                loginfo("proto file failed path:" + path)
                return False
            self.serialize_datas.append(p)
        return True

    # 枚举项不带作用域，名字全局唯一；错误码在所有 errordef 之间也不能重。
    # 枚举不参与序列化，rpcdata 字段用定宽整数，不能直接写枚举类型。
    def CheckEnums(self)->bool:
        items = {}
        codes = {}
        enum_names = set()
        for data in self.serialize_datas:
            if data.GetType() not in (SerializeDataType.ENUM, SerializeDataType.ERROR_DEF):
                continue
            enum_names.add(data.GetName())
            for member in data.members:
                if member.GetName() in items:
                    loginfo("duplicate enum item name=", member.GetName(), ",first=", items[member.GetName()], ",second=", data.GetName())
                    return False
                items[member.GetName()] = data.GetName()
                if data.GetType() != SerializeDataType.ERROR_DEF:
                    continue
                if member.GetValue() in codes:
                    loginfo("duplicate error code value=", member.GetValue(), ",first=", codes[member.GetValue()], ",second=", member.GetName())
                    return False
                codes[member.GetValue()] = member.GetName()
        for data in self.serialize_datas:
            if data.GetType() != SerializeDataType.RPC_DATA:
                continue
            for member in data.members:
                used = set(re.findall(r"[A-Za-z_][A-Za-z0-9_:]*", member.GetType())) & enum_names
                if used:
                    loginfo("rpcdata field cannot use enum type, use a fixed-width integer proto=", data.GetName(), ",field=", member.GetName(), ",enum=", sorted(used)[0])
                    return False
        return True

    def CheckRpcPayloads(self)->bool:
        for data in self.serialize_datas:
            if data.GetType() != SerializeDataType.RPC:
                continue
            for name in (data.GetReq(), data.GetRes()):
                if self.RpcDataByName(name) is None:
                    loginfo("rpc payload is not an rpcdata rpc=", data.GetName(), ",type=", name)
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
            if data is None or not data.has_json or data.is_key or parsed[1] in stack:
                return False
            stack.add(parsed[1])
            for member in data.members:
                if not member.json_name:
                    continue
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

    def IsDbScalar(self, parsed):
        if parsed[0] != "name":
            return False
        name = "std::string" if parsed[1] == "string" else parsed[1]
        return name in JSON_SCALARS

    # db 不穿透。标量按列读写。vector、map、rpcdata 整段当 blob，只检查能不能序列化，不要求里层标 db。
    def TypeDbOk(self, parsed):
        if parsed[0] == "bad":
            return False
        if parsed[0] == "name":
            name = "std::string" if parsed[1] == "string" else parsed[1]
            if name in JSON_SCALARS:
                return True
            data = self.RpcDataByName(name)
            return data is not None
        if parsed[0] == "vector":
            if parsed[1] == ("name", "bool"):
                return False
            return self.TypeDbOk(parsed[1])
        if parsed[0] == "map":
            return self.TypeDbOk(parsed[1]) and self.TypeDbOk(parsed[2])
        return False

    # [dbtype] 只换字节和文本列：string 任选；Bytes、PB blob 只能二进制类型；[dbjson] 只能文本类型；数值列不能换。
    # [dbkey] 只标整数、bool、string、Bytes；string、Bytes 当主键必须写定长的 [dbtype]，BLOB / TEXT 不能做主键。
    def CheckDbKeyAndType(self, data, member, parsed)->bool:
        name = None
        if parsed[0] == "name":
            name = "std::string" if parsed[1] == "string" else parsed[1]
        numeric = name in ORDERED_SCALARS and name != "std::string"
        if member.db_type:
            base = DbTypeBase(member.db_type)
            if member.db_json:
                allowed = base in DB_TEXT_TYPES
            elif name == "std::string":
                allowed = True
            elif numeric:
                allowed = False
            else:
                allowed = base in DB_BINARY_TYPES
            if not allowed:
                loginfo("dbtype does not fit field proto=", data.GetName(), ",field=", member.GetName(), ",type=", member.GetType(), ",dbtype=", member.db_type)
                return False
        if member.db_key:
            if member.db_json or name is None or name in ("float", "double") or not self.IsDbScalar(parsed):
                loginfo("dbkey needs an integer, bool, string or Bytes field proto=", data.GetName(), ",field=", member.GetName(), ",type=", member.GetType())
                return False
            if name in ("std::string", "Bytes") and (not member.db_type or DbTypeBase(member.db_type) not in DB_SIZED_TYPES):
                loginfo("dbkey string needs [dbtype VARBINARY n] or another sized type proto=", data.GetName(), ",field=", member.GetName())
                return False
        return True

    def CheckKeyComparators(self)->bool:
        for data in self.serialize_datas:
            if data.GetType() != SerializeDataType.RPC_DATA:
                continue
            if data.is_key and len(data.members) == 0:
                loginfo("key rpcdata need members name=", data.GetName())
                return False
            for member in data.members:
                parsed = ParseType(member.GetType())
                if HasVectorBool(parsed):
                    loginfo("vector<bool> is not supported, use vector<int32_t> owner=", data.GetName(), ",field=", member.GetName(), ",type=", member.GetType())
                    return False
                for key in IterMapKeys(parsed):
                    if not self.TypeOrdered(key, set()):
                        loginfo("map key has no comparator owner=", data.GetName(), ",field=", member.GetName(), ",key=", key)
                        return False
            if data.is_key:
                for member in data.members:
                    if not self.TypeOrdered(ParseType(member.GetType()), set([data.GetName()])):
                        loginfo("key rpcdata nested comparator missing name=", data.GetName(), ",field=", member.GetName(), ",type=", member.GetType())
                        return False
            if data.has_json:
                for member in data.members:
                    if not member.json_name:
                        continue
                    if not self.TypeJsonOk(ParseType(member.GetType()), set()):
                        loginfo("json field is not representable name=", data.GetName(), ",field=", member.GetName(), ",type=", member.GetType())
                        return False
            if data.has_db:
                key_bytes = 0
                for member in data.members:
                    if not member.db_name:
                        continue
                    parsed = ParseType(member.GetType())
                    if member.db_json:
                        # 只有真正入库的 vector、map、rpcdata 才能 dbjson，并且必须能写成 JSON。
                        blob = parsed[0] in ("vector", "map") or (parsed[0] == "name" and not self.IsDbScalar(parsed))
                        if parsed[0] == "vector" and parsed[1] == ("name", "bool"):
                            blob = False
                        if not blob or not self.TypeJsonOk(parsed, set()):
                            loginfo("dbjson needs a json vector, map or rpcdata proto=", data.GetName(), ",field=", member.GetName(), ",type=", member.GetType())
                            return False
                    elif not self.TypeDbOk(parsed):
                        loginfo("db field is not representable name=", data.GetName(), ",field=", member.GetName(), ",type=", member.GetType())
                        return False
                    if not self.CheckDbKeyAndType(data, member, parsed):
                        return False
                    if member.db_key:
                        key_bytes += DbKeyBytes(member.db_type)
                # 表是 utf8mb4 的 InnoDB，整个主键索引不超过 3072 字节，否则建表必然失败。
                if key_bytes > DB_KEY_MAX_BYTES:
                    loginfo("dbkey too long proto=", data.GetName(), ",bytes=", key_bytes, ",max=", DB_KEY_MAX_BYTES)
                    return False
            data.json_ok = data.has_json
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
            #排除同协议号。请求和回复共用这一个号。
            if(data.GetID() in self.proto_id_map):
                loginfo("CheckProtocol id repeat old_name=", self.proto_id_map[data.GetID()].GetName(), ",new_name=", data.GetName())
                return False
            self.proto_id_map[data.GetID()] = data
        if(serialize_type == SerializeDataType.RPC):
            req = data.GetReq()
            res = data.GetRes()
            if(req == res):
                loginfo("rpc request and response reuse the same rpcdata name=", data.GetName())
                return False
            used = getattr(self, "rpc_payload_types", None)
            if used is None:
                used = set()
                self.rpc_payload_types = used
            if(req in used or res in used):
                loginfo("rpcdata reused by another rpc name=", data.GetName(), ",request=", req, ",response=", res)
                return False
            used.add(req)
            used.add(res)
            
        self.type_name_map[data.GetName()] = data
        return True

