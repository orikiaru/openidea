from enum import Enum
from common import loginfo

class SerializeDataType(Enum):
    PROTOCOL = 1
    RPC_DATA = 2
    RPC = 3
    
class ClassMember:
    def __init__(self):
        self.name = None
        self.type = None
        self.value = None
    def GetName(self):
        return self.name
    def GetType(self):
        return self.type
    def GetValue(self):
        return self.value
    def ParseMember(self, member_xml):
        self.name = member_xml.get("name")
        self.type = member_xml.get("type")
        self.value = member_xml.get("default")
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
    def GetTimeOut(self):
        return self.timeout
    def GetName(self):
        return self.name
    def GetDataName(self):
        return self.name.lower()
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
            member.ParseMember(m_xml)
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
        return True  
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
        self.ParseCommon()
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
        return self.ParseMember()

    #解析RPC
    def ParseRpc(self)->bool:
        self.id = self.xml_data.get("id")
        self.name = self.xml_data.get("name")
        if(not self.id or not self.name):
            loginfo("parseprotocol error not id or name, content=", self.xml_data.text, "id=", self.id, ",name=", self.name)
            return False
        timeout = self.xml_data.get("timeout")
        if(timeout):
            self.timeout = timeout
        return self.ParseMember()
    
    #解析一些通用内容
    def ParseCommon(self)->bool:
        pass
    