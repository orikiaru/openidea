from enum import Enum
import re
from common import loginfo
from field_naming import ValidFieldName

DB_COLUMN_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
JSON_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_:]*")
# [dbtype] 只能换成下面这些列类型。带长度的必须写长度，其余不能写。
DB_SIZED_TYPES = {"VARCHAR": 65535, "CHAR": 255, "VARBINARY": 65535, "BINARY": 255}
DB_TEXT_TYPES = {"VARCHAR", "CHAR", "TEXT", "MEDIUMTEXT", "LONGTEXT"}
DB_BINARY_TYPES = {"VARBINARY", "BINARY", "BLOB", "MEDIUMBLOB", "LONGBLOB"}
DB_TYPE_TEXT = re.compile(r"([A-Za-z]+)(?:\((\d+)\))?")

# 返回规范写法，例如 VARBINARY(64)、LONGTEXT；不合法返回 None。
def NormalizeDbType(name, length):
    upper = str(name).upper()
    if upper not in DB_TEXT_TYPES and upper not in DB_BINARY_TYPES:
        return None
    if upper in DB_SIZED_TYPES:
        if length is None or not 1 <= length <= DB_SIZED_TYPES[upper]:
            return None
        return "%s(%d)" % (upper, length)
    if length is not None:
        return None
    return upper

def DbTypeBase(db_type):
    return db_type.split("(")[0]

def ParseBoolAttr(text):
    if text is None or text == "":
        return False
    lowered = str(text).lower()
    if lowered in ("true", "1"):
        return True
    if lowered in ("false", "0"):
        return False
    return None

class SerializeDataType(Enum):
    PROTOCOL = 1
    RPC_DATA = 2
    RPC = 3
    # 只生成 C 枚举，不参与序列化。
    ENUM = 4
    ERROR_DEF = 5
    
class ClassMember:
    def __init__(self):
        self.name = None
        self.type = None
        self.value = None
        self.seq = None
        self.json_name = None
        self.db_name = None
        self.no_json = False
        self.no_db = False
        self.json_marked = False
        self.db_marked = False
        self.db_json = False
        self.db_key = False
        self.db_type = None
    def GetName(self):
        return self.name
    def GetType(self):
        return self.type
    def GetValue(self):
        return self.value
    def GetSeq(self):
        return self.seq
    def ParseMember(self, member_xml):
        self.name = member_xml.get("name")
        json_text = member_xml.get("json")
        if json_text:
            self.json_name = json_text
        db_text = member_xml.get("db")
        if db_text:
            self.db_name = db_text
        no_json = ParseBoolAttr(member_xml.get("nojson"))
        no_db = ParseBoolAttr(member_xml.get("nodb"))
        if no_json is None or no_db is None:
            loginfo("ParseMember codec flag invalid name=", self.name, ",nojson=", member_xml.get("nojson"), ",nodb=", member_xml.get("nodb"))
            return False
        self.no_json = no_json
        self.no_db = no_db
        db_json = ParseBoolAttr(member_xml.get("dbjson"))
        if db_json is None:
            loginfo("ParseMember dbjson flag invalid name=", self.name, ",dbjson=", member_xml.get("dbjson"))
            return False
        self.db_json = db_json
        db_key = ParseBoolAttr(member_xml.get("dbkey"))
        if db_key is None:
            loginfo("ParseMember dbkey flag invalid name=", self.name, ",dbkey=", member_xml.get("dbkey"))
            return False
        self.db_key = db_key
        db_type_text = member_xml.get("dbtype")
        if db_type_text:
            matched = DB_TYPE_TEXT.fullmatch(db_type_text.strip())
            self.db_type = NormalizeDbType(matched.group(1), int(matched.group(2)) if matched.group(2) else None) if matched else None
            if self.db_type is None:
                loginfo("ParseMember dbtype invalid name=", self.name, ",dbtype=", db_type_text)
                return False
        self.type = member_xml.get("type")
        self.value = member_xml.get("default")
        seq_text = member_xml.get("seq")
        if seq_text is not None and seq_text != "":
            try:
                self.seq = int(seq_text)
            except ValueError:
                loginfo("ParseMember seq invalid name=", self.name, ",seq=", seq_text)
                return False
            if self.seq < 0:
                loginfo("ParseMember seq must >= 0 name=", self.name, ",seq=", self.seq)
                return False
        return True

class SerializeData:
    def __init__(self, type, xml_data):
        self.xml_data = xml_data
        self.type = type
        self.members = []
        self.id = ""
        self.name = ""
        self.proto_type = ""
        self.timeout = "30"
        self.res_id = ""
        self.is_key = False
        self.has_json = False
        self.has_db = False
        self.json_ok = False

    def GenLogHeader(self):
        log = 'type={},id={},name={},type_enum={}'.format(SerializeDataType(self.type), self.id, self.name, self.GetProtoType())
        return log
    def GetProtoType(self):
        #assert(self.type == SerializeDataType.RPC or self.type == SerializeDataType.PROTOCOL)
        if(self.proto_type == ""):
            if(self.GetType() == SerializeDataType.PROTOCOL):
                self.proto_type = "PROTOCOL_TYPE_{}".format(self.name).upper()
            elif(self.GetType() == SerializeDataType.RPC):
                self.proto_type = "RPC_TYPE_{}".format(self.name).upper()
        return self.proto_type   
        
    def IsProtocol(self):
        return self.GetType() == SerializeDataType.RPC or self.GetType() == SerializeDataType.PROTOCOL
    def GetID(self):
        return self.id
    def GetResID(self):
        return self.res_id
    def GetTimeOut(self):
        return self.timeout
    def GetName(self):
        return self.name
    def GetDataName(self):
        return "oi_rpcdata_" + self.name.lower() + ".h"
    def GetProtoFileName(self):
        suffix = ""
        if(self.type == SerializeDataType.RPC):
            suffix = ".hrp"
        elif(self.type == SerializeDataType.PROTOCOL):
            suffix = ".hpp"
        str = self.name + suffix
        return str.lower()
    def GetType(self):
        return self.type
    def GetReq(self):
        if(self.type == SerializeDataType.RPC):
            assert(len(self.members) == 2)
            return self.members[0].type
        return ""
    def GetRes(self):
        if(self.type == SerializeDataType.RPC):
            assert(len(self.members) == 2)
            return self.members[1].type
        return ""

    def ParseMember(self)->bool:
        if(self.type == SerializeDataType.PROTOCOL or self.type == SerializeDataType.RPC_DATA):
            return self.ParseClassMember()
        elif(self.type == SerializeDataType.RPC):
            return self.ParseRpcMember()
        else:
            return False   
    #解析protocol和rpcdata
    def ParseClassMember(self)->bool:
        if(self.type != SerializeDataType.PROTOCOL and self.type != SerializeDataType.RPC_DATA):
            loginfo("ParseClassMember error id=", self.GetID(), ",name=",self.GetName())
            return False
        find_default = False
        for m_xml in self.xml_data.findall("member"):
            member = ClassMember()
            if not member.ParseMember(m_xml):
                return False
            if(find_default and not member.GetValue()):
                #default用于填充c++构造参数,默认参数必须从后向前
                loginfo("ParseProtocol error Proto[", self.name, "], member[", member.GetName(), "] need default data")
                return False
            if(member.GetValue()):
                find_default = True
            for m in self.members:
                if(m.GetName() == member.GetName()):
                    loginfo("ParseProtocol error Proto[", self.name, "], Variable with the same name [", member.GetName(),"]")
                    return False
            self.members.append(member)
        return self.AssignFieldSeq()

    # json / db 标在结构上。没标的结构不生成对应方法。
    # 字段默认都参加，序列化名就是字段名。[json]、[db] 的名字可省略。
    # JSON 穿透。[dbjson] 才把 vector、map、rpcdata 按 JSON 存进库，否则仍是 PB blob。
    def ApplyCodecMarkers(self):
        if self.has_json and self.is_key:
            loginfo("key rpcdata cannot generate json name=", self.name)
            return False
        json_names = set()
        db_names = set()
        for member in self.members:
            # 没标 json 的结构不能出现 json 标记。nodb 字段不再是数据库字段，不能再带 db / dbjson。
            if member.no_json and (member.json_name or member.json_marked):
                loginfo("json and nojson proto=", self.name, ",field=", member.name)
                return False
            if member.no_db and (member.db_name or member.db_marked or member.db_json or member.db_key or member.db_type):
                loginfo("db and nodb proto=", self.name, ",field=", member.name)
                return False
            if not self.has_json and (member.no_json or member.json_name or member.json_marked):
                loginfo("field json marker requires rpcdata json proto=", self.name, ",field=", member.name)
                return False
            if not self.has_db and (member.no_db or member.db_name or member.db_marked or member.db_json or member.db_key or member.db_type):
                loginfo("db marker requires db rpcdata proto=", self.name, ",field=", member.name)
                return False
            if self.has_json and not member.no_json and not member.json_name:
                member.json_name = member.name
            if not self.has_json or member.no_json:
                member.json_name = None
            if self.has_db and not member.no_db and not member.db_name:
                member.db_name = member.name
            if not self.has_db or member.no_db:
                member.db_name = None
            if member.json_name:
                if not JSON_NAME.fullmatch(member.json_name):
                    loginfo("invalid json name proto=", self.name, ",field=", member.name, ",json=", member.json_name)
                    return False
                if member.json_name in json_names:
                    loginfo("duplicate JSON name proto=", self.name, ",json=", member.json_name)
                    return False
                json_names.add(member.json_name)
            if member.db_name:
                if not DB_COLUMN_NAME.fullmatch(member.db_name):
                    loginfo("invalid db column proto=", self.name, ",field=", member.name, ",db=", member.db_name)
                    return False
                if member.db_name in db_names:
                    loginfo("duplicate db column proto=", self.name, ",db=", member.db_name)
                    return False
                db_names.add(member.db_name)
        if self.has_json and not json_names:
            loginfo("json rpcdata has no json field name=", self.name)
            return False
        if self.has_db and not db_names:
            loginfo("db rpcdata has no db field name=", self.name)
            return False
        self.json_ok = self.has_json
        return True

    # 字段号必须写在协议里。缺号、重号都是协议错，生成器不补。
    def AssignFieldSeq(self):
        used = set()
        for member in self.members:
            if not ValidFieldName(member.type, member.name):
                loginfo("invalid field name proto=", self.name, ",field=", member.name, ",type=", member.type)
                return False
            if member.seq is None:
                loginfo("field number required proto=", self.name, ",field=", member.GetName())
                return False
            if member.seq in used:
                loginfo("duplicate field number proto=", self.name, ",field=", member.GetName(), ",seq=", member.seq)
                return False
            used.add(member.seq)
        return self.ApplyCodecMarkers()  
    #解析Rpc的Req和Res
    def ParseRpcMember(self)->bool:
        if(self.type != SerializeDataType.RPC):
            loginfo("ParseRpcMember error id=", self.GetID(), ",name=",self.GetName())
            return False
        
        req = self.xml_data.get("request")
        res = self.xml_data.get("response")
        if(not req or not res):
            loginfo("ParseRpcMember error not request or response rpc_name=", self.GetName(), ",rpc_id=", self.GetID())
            return False
        
        #复用members存储req和res
        member_req = ClassMember()
        member_req.name = "req"
        member_req.type = req
        member_res = ClassMember()
        member_res.name = "res"
        member_res.type = res
        
        self.members.append(member_req)
        self.members.append(member_res)

        return True
    
    def Parse(self)->bool:
        ret = False
        if(self.type == SerializeDataType.PROTOCOL):
            ret = self.ParseProtocol()
        elif(self.type == SerializeDataType.RPC_DATA):
            ret = self.ParseRpcData()
        elif(self.type == SerializeDataType.RPC):
            ret = self.ParseRpc()
        else:
            loginfo("SerializeData::Parse Error Invalid self.type=", self.type)
            return False
        if(ret):
            loginfo(self.GenLogHeader(), " ParseFinish")
        else:
            loginfo(self.GenLogHeader(), " ParseError")
        return ret    
    #解析协议      
    def ParseProtocol(self)->bool:
        self.id = self.xml_data.get("id")
        self.name = self.xml_data.get("name")
        if(not self.id or not self.name):
            loginfo("parseprotocol error not id or name, content=", self.xml_data.text, "id=", self.id, ",name=", self.name)
            return False
        return self.ParseMember()
    #解析RPCData
    def ParseRpcData(self)->bool:
        self.name = self.xml_data.get("name")
        if(not self.name):
            loginfo("ParseRpcData error not id or name, content=", self.xml_data.text, "id=", self.id, ",name=", self.name)
            return False
        key_text = self.xml_data.get("key")
        if key_text is None or key_text == "":
            self.is_key = False
        elif str(key_text).lower() in ("true", "1"):
            self.is_key = True
        elif str(key_text).lower() in ("false", "0"):
            self.is_key = False
        else:
            loginfo("ParseRpcData key invalid name=", self.name, ",key=", key_text)
            return False
        has_json = ParseBoolAttr(self.xml_data.get("json"))
        has_db = ParseBoolAttr(self.xml_data.get("db"))
        if has_json is None or has_db is None:
            loginfo("ParseRpcData codec flag invalid name=", self.name, ",json=", self.xml_data.get("json"), ",db=", self.xml_data.get("db"))
            return False
        self.has_json = has_json
        self.has_db = has_db
        return self.ParseMember()

    #解析RPC
    def ParseRpc(self)->bool:
        self.id = self.xml_data.get("id")
        self.res_id = self.xml_data.get("res_id") or self.id
        self.name = self.xml_data.get("name")
        if(not self.id or not self.name):
            loginfo("parseprotocol error not id or name, id=", self.id, ",name=", self.name)
            return False
        timeout = self.xml_data.get("timeout")
        if(timeout):
            self.timeout = timeout
        return self.ParseMember()
    