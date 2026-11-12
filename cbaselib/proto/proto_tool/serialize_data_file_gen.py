from serialize_data_parse import SerializeData
from serialize_data_parse import SerializeDataType
import re

def QualType(t):
    return re.sub(r'\bBytes\b', '::OI::Base::Bytes', t)

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
                str = str.replace(r'__TYPE__', QualType(m.type))
                str = str.replace(r'__NAME__',m.name)
                if(m.value):
                    str = str.replace(r'__VALUE__',m.value)
                self.member_init_param_list += str
            self.member_init_param_list = self.member_init_param_list[0:self.member_init_param_list.rindex(',')]
        return self.member_init_param_list       
    
    def GetFieldSeq(self, index, member):
        if member.seq is None:
            return index
        return member.seq

    # 每个字段一条：seq(从0起，与字段顺序对应) + size + data
    def GetPbSerialize(self):
        lines = []
        for index, m in enumerate(self.data.members):
            lines.append("        os.WritePb({}, {});".format(self.GetFieldSeq(index, m), m.name))
        if(len(lines) == 0):
            return ""
        return "\n".join(lines) + "\n"

    def GetPbDeserialize(self):
        lines = []
        for m in self.data.members:
            if("vector" in m.type or "map" in m.type):
                lines.append("        {}.clear();".format(m.name))
        lines.append("        while (os.RemainSize() > 0)")
        lines.append("        {")
        lines.append("            uint32_t field_seq = 0;")
        lines.append("            uint32_t field_size = 0;")
        lines.append("            os.ReadPbHeader(field_seq, field_size);")
        lines.append("            switch (field_seq)")
        lines.append("            {")
        for index, m in enumerate(self.data.members):
            lines.append("            case {}: os.ReadPb(field_size, {}); break;".format(self.GetFieldSeq(index, m), m.name))
        lines.append("            default: os.SkipPb(field_size); break;")
        lines.append("            }")
        lines.append("        }")
        lines.append("        return os;")
        return "\n".join(lines) + "\n"
        
    def GetMemberDeclareList(self):
        if(self.member_declare_list == ""):
            for m in self.data.members:
                #" m.type m.name{}; " 标量没出现在包里时要是 0，不能是未初始化的值
                self.member_declare_list += QualType(m.type) + r' ' + m.name + r'{};' + '\n    ' 
        return self.member_declare_list        
             
    def DoReplace(self, file_data:str):
        init = self.GetMemberInitList()
        init_param = self.GetMemberInitParamList()
        class_name = self.data.GetName()
        data_file_name = self.data.GetDataName()
        member_declare = self.GetMemberDeclareList()
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
        file_data = file_data.replace("__PB_SERIALIZE__", self.GetPbSerialize())
        file_data = file_data.replace("__PB_DESERIALIZE__", self.GetPbDeserialize())
        file_data = file_data.replace("__PROTO_TYPE__", proto_type)
        file_data = file_data.replace("__PROTO_ID__", proto_id)
        file_data = file_data.replace("__REQ_TYPE__", req_type)
        file_data = file_data.replace("__RES_TYPE__", res_type)
        file_data = file_data.replace("__req_data_type__", req_type.lower())
        file_data = file_data.replace("__res_data_type__", res_type.lower())
        
        return file_data
        
    def GenDataFile(self):
        # rpc 的请求和回复已经各是一个 rpcdata。不再另写一层类，也不把函数体 #include 进类里。
        if(self.data.GetType() != SerializeDataType.RPC_DATA):
            return ""
        return self.GenRpcDataFile()

    def GenRpcDataFile(self):
        assert(self.data.GetType() == SerializeDataType.RPC_DATA)
        file_data = ""
        file_data += '#pragma once\n#include "oi_rpc.h"\n__JSON_INCLUDE____DB_INCLUDE__'
        file_data += '''
namespace OI { namespace RpcData {
class __CLASS_NAME__ : public ::OI::Rpc::RpcData
{
public:
    __MEMBER_DECLARE__
    explicit __CLASS_NAME__ (){}
    __CLASS_NAME__ ( __MEMBER_INIT_PARAM_LIST__ )
    {
        __MEMBER_INIT_LIST__
    }
    virtual ::OI::Base::BytesStream& serialize(::OI::Base::BytesStream& os) const
    {
__PB_SERIALIZE__        return os;
    }
    virtual ::OI::Base::BytesStream& deserialize(::OI::Base::BytesStream& os)
    {
__PB_DESERIALIZE__    }
__COMPARE____JSON_METHODS____DB_METHODS__};
}}
'''
        file_data = self.DoReplace(file_data)
        if self.data.has_json:
            file_data = file_data.replace("__JSON_INCLUDE__", '#include "oi_json.h"\n#include <memory>\n#include <string>\n')
            file_data = file_data.replace("__JSON_METHODS__", self.GetJsonMethods())
        else:
            file_data = file_data.replace("__JSON_INCLUDE__", "")
            file_data = file_data.replace("__JSON_METHODS__", "")
        if self.data.has_db:
            db_include = '#include "oi_db_row.h"\n'
            if any(member.db_json for member in self.data.members):
                db_include = '#include "oi_json.h"\n' + db_include
            file_data = file_data.replace("__DB_INCLUDE__", db_include)
            file_data = file_data.replace("__DB_METHODS__", self.GetDbMethods())
        else:
            file_data = file_data.replace("__DB_INCLUDE__", "")
            file_data = file_data.replace("__DB_METHODS__", "")
        file_data = file_data.replace("__COMPARE__", self.GetCompareMethod())
        return file_data

    def GetCompareMethod(self):
        if not self.data.is_key:
            return ""
        lines = []
        lines.append("    bool operator<(const %s& rhs) const" % self.class_name)
        lines.append("    {")
        for m in self.data.members:
            lines.append("        if (%s < rhs.%s) return true;" % (m.name, m.name))
            lines.append("        if (rhs.%s < %s) return false;" % (m.name, m.name))
        lines.append("        return false;")
        lines.append("    }")
        return "\n".join(lines) + "\n"

    def GetJsonMethods(self):
        lines = []
        lines.append("    bool FromJson(const Json::Value& in)")
        lines.append("    {")
        lines.append("        if (!in.isObject()) return false;")
        lines.append("        %s decoded(*this);" % self.class_name)
        for m in self.data.members:
            if not m.json_name:
                continue
            lines.append('        if (in.isMember("%s") && !::OI::Rpc::FromJsonValue(in["%s"], decoded.%s)) return false;' % (m.json_name, m.json_name, m.name))
        lines.append("        *this = std::move(decoded);")
        lines.append("        return true;")
        lines.append("    }")
        lines.append("    bool FromJson(const std::string& text)")
        lines.append("    {")
        lines.append("        Json::CharReaderBuilder builder;")
        lines.append("        Json::Value root;")
        lines.append("        std::string errs;")
        lines.append("        const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());")
        lines.append("        const char* begin = text.data();")
        lines.append("        if (!reader->parse(begin, begin + text.size(), &root, &errs)) return false;")
        lines.append("        return FromJson(root);")
        lines.append("    }")
        lines.append("    void ToJson(Json::Value& out) const")
        lines.append("    {")
        lines.append("        out = Json::Value(Json::objectValue);")
        for m in self.data.members:
            if not m.json_name:
                continue
            lines.append('        ::OI::Rpc::ToJsonValue(out["%s"], %s);' % (m.json_name, m.name))
        lines.append("    }")
        lines.append("    std::string ToJson() const")
        lines.append("    {")
        lines.append("        Json::Value out;")
        lines.append("        ToJson(out);")
        lines.append("        Json::StreamWriterBuilder builder;")
        lines.append('        builder["indentation"] = "";')
        lines.append("        return Json::writeString(builder, out);")
        lines.append("    }")
        return "\n".join(lines) + "\n"

    def DbValueKind(self, member):
        if member.db_json:
            return "dbjson"
        name = re.sub(r"\s+", "", member.type or "")
        if name in ("string", "std::string"):
            return "string"
        if name == "Bytes":
            return "bytes"
        if name in ("int8_t", "int16_t", "int32_t", "int64_t"):
            return "signed"
        if name in ("uint8_t", "uint16_t", "uint32_t", "uint64_t"):
            return "unsigned"
        if name == "bool":
            return "bool"
        if name == "float":
            return "float"
        if name == "double":
            return "double"
        # 头一层剩下的就是 vector、map、rpcdata，整段收成 blob。
        return "blob"

    def DbReadCall(self, member):
        kind = self.DbValueKind(member)
        if kind == "bool":
            return "::OI::Db::FromDbBool(*stCell, stDecoded.%s)" % member.name
        if kind in ("float", "double"):
            return "::OI::Db::FromDbReal(*stCell, stDecoded.%s)" % member.name
        if kind in ("string", "bytes"):
            return "::OI::Db::FromDbText(*stCell, stDecoded.%s)" % member.name
        if kind == "dbjson":
            return "::OI::Rpc::FromJsonText(std::string(reinterpret_cast<const char*>(stCell->data()), stCell->size()), stDecoded.%s)" % member.name
        if kind == "blob":
            return "::OI::Db::FromDbBlob(*stCell, stDecoded.%s)" % member.name
        if kind == "signed" or kind == "unsigned":
            return "::OI::Db::FromDbInteger(*stCell, stDecoded.%s)" % member.name
        raise RuntimeError("db field is not a scalar or blob: " + member.name)

    def DbWriteLine(self, member):
        kind = self.DbValueKind(member)
        if kind == "signed" or kind == "bool":
            value = member.name if kind == "signed" else "(%s ? 1 : 0)" % member.name
            return "        vParameters.emplace_back(static_cast<int64_t>(%s));" % value
        if kind == "unsigned":
            return "        vParameters.emplace_back(static_cast<uint64_t>(%s));" % member.name
        if kind == "float":
            return "        vParameters.emplace_back(static_cast<double>(%s));" % member.name
        if kind == "double" or kind == "string":
            return "        vParameters.emplace_back(%s);" % member.name
        if kind == "bytes":
            return "        vParameters.emplace_back(::OI::Db::Blob(reinterpret_cast<const uint8_t*>(%s.Data()), reinterpret_cast<const uint8_t*>(%s.Data()) + %s.Size()));" % (member.name, member.name, member.name)
        if kind == "dbjson":
            return "        vParameters.emplace_back(::OI::Rpc::ToJsonText(%s));" % member.name
        if kind == "blob":
            return "        vParameters.emplace_back(::OI::Db::ToDbBlob(%s));" % member.name
        raise RuntimeError("db field is not a scalar or blob: " + member.name)

    def GetDbMethods(self):
        members = [member for member in self.data.members if member.db_name]
        lines = []
        lines.append("    static const char* DbColumnList()")
        lines.append("    {")
        lines.append('        return "%s";' % ",".join("`%s`" % member.db_name for member in members))
        lines.append("    }")
        lines.append("    static const char* DbPlaceholders()")
        lines.append("    {")
        lines.append('        return "%s";' % ",".join(["?"] * len(members)))
        lines.append("    }")
        lines.append("    void AppendDbParameters(std::vector<::OI::Db::Parameter>& vParameters) const")
        lines.append("    {")
        for member in members:
            lines.append(self.DbWriteLine(member))
        lines.append("    }")
        lines.append("    bool FromDbRow(const std::vector<std::string>& vColumns, const std::vector<std::optional<::OI::Db::Blob>>& vRow)")
        lines.append("    {")
        lines.append("        if (vColumns.size() != vRow.size())")
        lines.append("        {")
        lines.append("            return false;")
        lines.append("        }")
        lines.append("        %s stDecoded(*this);" % self.class_name)
        lines.append("        bool vSeen[%d] = {};" % len(members))
        lines.append("        for (size_t iIndex = 0; iIndex < vColumns.size(); ++iIndex)")
        lines.append("        {")
        lines.append("            const std::string& sColumn = vColumns[iIndex];")
        lines.append("            const std::optional<::OI::Db::Blob>& stCell = vRow[iIndex];")
        opener = "if"
        for index, member in enumerate(members):
            lines.append('            %s (sColumn == "%s")' % (opener, member.db_name))
            lines.append("            {")
            lines.append("                if (vSeen[%d] || !stCell || !%s)" % (index, self.DbReadCall(member)))
            lines.append("                {")
            lines.append("                    return false;")
            lines.append("                }")
            lines.append("                vSeen[%d] = true;" % index)
            lines.append("            }")
            opener = "else if"
        lines.append("        }")
        lines.append("        *this = std::move(stDecoded);")
        lines.append("        return true;")
        lines.append("    }")
        return "\n".join(lines) + "\n"
