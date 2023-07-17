from serialize_data_parse import SerializeData
from serialize_data_parse import SerializeDataType

common_include = '#pragma once\n#include "net_header.h"' + '\n' 

class SerializeDataFileGen:
    def __init__(self, data: SerializeData):
        self.data = data
        self.class_name = data.GetName()
        self.member_init_param_list = ""            # Protocol(const A& l_a = A())
        self.member_init_list = ""                  #" member = l_member "      构造赋值用
        self.member_serialize_list = ""             #" Left XXX Right "         XXX为自定义符号 << >>   序列化用
        self.member_declare_list = ""               #" type a"
    ##  self.name = None
    #   self.type = None
    #   self.value = None
    def GetMemberInitList(self):
        if(self.member_init_list == ""):
            for m in self.data.members:
                #" member = l_member;"
                self.member_init_list += m.name + r' = l_' + m.name + r';' + '\n        ' 
        return self.member_init_list        
    
    def GetMemberInitParamList(self):
        if(self.member_init_param_list == ""):
            for m in self.data.members:
                # const m.type & l_m.name = m.value , const member2 &l_member2 = m.value
                str = r'const __TYPE__ & l___NAME__'
                if(m.value):
                    str += r' = __VALUE__'
                str += r', '
                str = str.replace(r'__TYPE__',m.type)
                str = str.replace(r'__NAME__',m.name)
                if(m.value):
                    str = str.replace(r'__VALUE__',m.value)
                self.member_init_param_list += str
            self.member_init_param_list = self.member_init_param_list[0:self.member_init_param_list.rindex(',')]
        return self.member_init_param_list       
    
    def GetMemberSerializeList(self, op):
        if(self.member_serialize_list == ""):
            self.member_serialize_list = "os "
            for m in self.data.members:
                #" os << m.name << m.name2;"
                #" os >> m.name >> m.name3;"
                self.member_serialize_list += r' __OPERATOR__ ' + m.name
            self.member_serialize_list += ";"
        str = self.member_serialize_list
        str = str.replace('__OPERATOR__', op)
        return str
        
    def GetMemberDeclareList(self):
        if(self.member_declare_list == ""):
            for m in self.data.members:
                #" m.type m.name "
                self.member_declare_list += m.type + r' ' + m.name + r';' + '\n    ' 
        return self.member_declare_list        
             
    def DoReplace(self, file_data:str):
        init = self.GetMemberInitList()
        init_param = self.GetMemberInitParamList()
        class_name = self.data.GetName()
        data_file_name = self.data.GetDataName()
        member_declare = self.GetMemberDeclareList()
        member_serialize = self.GetMemberSerializeList("<<")
        member_unserialize = self.GetMemberSerializeList(">>")
        proto_type = self.data.GetProtoType()
        req_type = self.data.GetReq()
        res_type = self.data.GetRes()
        proto_id = self.data.GetID()
        time_out = self.data.GetTimeOut()

        file_data = file_data.replace("__TIME_OUT__", time_out)
        file_data = file_data.replace("__CLASS_NAME__", class_name)
        file_data = file_data.replace("__MEMBER_INIT_PARAM_LIST__", init_param)
        file_data = file_data.replace("__MEMBER_INIT_LIST__", init)
        file_data = file_data.replace("__DATA_FILE_NAME__", data_file_name)
        file_data = file_data.replace("__MEMBER_DECLARE__", member_declare)
        file_data = file_data.replace("__MEMBER_SERIALIZE__", member_serialize)
        file_data = file_data.replace("__MEMBER_UNSERIALIZE__", member_unserialize)
        file_data = file_data.replace("__PROTO_TYPE__", proto_type)
        file_data = file_data.replace("__PROTO_ID__", proto_id)
        file_data = file_data.replace("__REQ_TYPE__", req_type)
        file_data = file_data.replace("__RES_TYPE__", res_type)
        file_data = file_data.replace("__req_data_type__", req_type.lower())
        file_data = file_data.replace("__res_data_type__", res_type.lower())
        
        return file_data
        
    def GenHeaderFile(self):
        if(self.data.GetType() == SerializeDataType.PROTOCOL):
            return self.GenProtocolHeaderFile()
        if(self.data.GetType() == SerializeDataType.RPC):
            return self.GenRpcHeaderFile()
        return ""
    def GenDataFile(self):
        if(self.data.GetType() == SerializeDataType.PROTOCOL):
            return self.GenProtocolDataFile()
        if(self.data.GetType() == SerializeDataType.RPC_DATA):
            return self.GenRpcDataFile()
        if(self.data.GetType() == SerializeDataType.RPC):
            return self.GenRpcIncludeDataFile()
        return ""   

    def GenRpcHeaderFile(self):
        assert(self.data.GetType() == SerializeDataType.RPC)
        file_data = ""
        file_data += common_include
        file_data += '''
#include "rpc.h"
#include "__req_data_type__"
#include "__res_data_type__"     
class __CLASS_NAME__ : public Rpc
{
public:
    #include "__DATA_FILE_NAME__"
    void DoRequest(SessionManager* mgr, sid_t sid)
    {
        //__REQ_TYPE__ * req = dynamic_cast<__REQ_TYPE__*>(_req);
        //__RES_TYPE__ * res = dynamic_cast<__RES_TYPE__*>(_res);
    }
    void DoResponse(SessionManager* mgr, sid_t sid)
    {
        //__REQ_TYPE__ * req = dynamic_cast<__REQ_TYPE__*>(_req);
        //__RES_TYPE__ * res = dynamic_cast<__RES_TYPE__*>(_res);
    }
    void OnTimeOut()
    {
        //__REQ_TYPE__ * req = dynamic_cast<__REQ_TYPE__*>(_req);
    }
};
'''   
        file_data = self.DoReplace(file_data)
        return file_data

    def GenRpcIncludeDataFile(self):
        #TODO
        assert(self.data.GetType() == SerializeDataType.RPC)
        file_data = '''
    __CLASS_NAME__ (RpcData* req, RpcData* res): Rpc( __PROTO_TYPE__ , req, res){}
    __CLASS_NAME__ (const __CLASS_NAME__& that): Rpc(that){}
    enum { __PROTO_TYPE__ = __PROTO_ID__, };
    static proto_type_t RpcType() { return __PROTO_TYPE__; }
    int TimeOut() { return __TIME_OUT__; }
    virtual void SetReq(RpcData* req)
    {
        SAFE_DELETE(_req);
        __REQ_TYPE__* t_req = dynamic_cast<__REQ_TYPE__*>(req->Clone());
        assert(_req = t_req);
    }   
    Protocol* Clone() { return new __CLASS_NAME__(*this); }        
'''

        file_data = self.DoReplace(file_data)
        return file_data
           
    def GenRpcDataFile(self):
        assert(self.data.GetType() == SerializeDataType.RPC_DATA)
        file_data = ""
        file_data += common_include
        file_data += '''
class __CLASS_NAME__ : public RpcData
{
public:
    __MEMBER_DECLARE__
    explicit __CLASS_NAME__ (){}
    __CLASS_NAME__ ( __MEMBER_INIT_PARAM_LIST__ )
    {
        __MEMBER_INIT_LIST__
    }
    virtual BytesStream& serialize(BytesStream& os) const
    {
        return __MEMBER_SERIALIZE__
    }
    virtual BytesStream& deserialize(BytesStream& os)
    {
        return __MEMBER_UNSERIALIZE__
    }
    virtual RpcData* Clone() { return new __CLASS_NAME__(*this); }
};
'''        
        file_data = self.DoReplace(file_data)
        return file_data

    def GenProtocolHeaderFile(self):
        assert(self.data.GetType() == SerializeDataType.PROTOCOL)
        file_data = common_include
        file_data += '''
class __CLASS_NAME__ : public Protocol
{
public:
    #include "__DATA_FILE_NAME__"
    virtual void Handle( SessionManager* mgr, sid_t sid)
    {
        //TODO Handle
    }
};
'''

    
        file_data = self.DoReplace(file_data)
        return file_data

    def GenProtocolDataFile(self):
        file_data = '''
    __MEMBER_DECLARE__ 
    explicit __CLASS_NAME__ (): Protocol( __PROTO_TYPE__ ){}
    __CLASS_NAME__ ( __MEMBER_INIT_PARAM_LIST__ ) : Protocol( __PROTO_TYPE__ )
    { 
        __MEMBER_INIT_LIST__ 
    } 
    enum { __PROTO_TYPE__ = __PROTO_ID__, };
    virtual BytesStream& serialize(BytesStream& os) const 
    { 
        return __MEMBER_SERIALIZE__ 
    } 
    virtual BytesStream& deserialize(BytesStream& os) 
    { 
        return __MEMBER_UNSERIALIZE__ 
    } 
    virtual Protocol* Clone()	{  return new __CLASS_NAME__(*this);  }
'''
        file_data = self.DoReplace(file_data)
        return file_data    
    
    def GenStubFile(self):
        file_data = ""
        if(self.data.GetType() == SerializeDataType.PROTOCOL):
            file_data += "ProtocolStubs::GetInstance().RegisterStub(new __CLASS_NAME__ ( ));" + '\n'
        elif(self.data.GetType() == SerializeDataType.RPC):
            file_data += "ProtocolStubs::GetInstance().RegisterStub(new __CLASS_NAME__ ( new __REQ_TYPE__(), new __RES_TYPE__()));" + '\n'
        file_data = self.DoReplace(file_data)
        return file_data
