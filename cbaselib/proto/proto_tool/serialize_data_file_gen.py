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
        # 没有字段时没有参数构造函数。空串再 rindex 会在生成时空包协议处崩掉。
        if not self.data.members:
            return ""
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
        fields = sorted((self.GetFieldSeq(index, member), member)
                        for index, member in enumerate(self.data.members))
        lines = ["        static constexpr std::array<::OI::Base::PbField, %d> fields = {{" % len(fields)]
        for sequence, member in fields:
            lines.append("            OI_PB_FIELD(%s, %d, %s),"
                         % (self.class_name, sequence, member.name))
        lines.append("        }};")
        lines.append("        std::array<uint8_t, %d> seen{};" % len(fields))
        lines.append("        return os.ReadFields(this, fields, seen);")
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
    ::OI::Base::BytesStream& serialize(::OI::Base::BytesStream& os) const override
    {
__PB_SERIALIZE__        return os;
    }
    ::OI::Base::BytesStream& deserialize(::OI::Base::BytesStream& os) override
    {
__PB_DESERIALIZE__    }
__COMPARE____JSON_METHODS____DB_METHODS__};
}}
'''
        # 无字段再留参数构造函数会和上面的默认构造重复。有字段的输出保持原样。
        if not self.data.members:
            ctor = "    __CLASS_NAME__ ( __MEMBER_INIT_PARAM_LIST__ )\n    {\n        __MEMBER_INIT_LIST__\n    }\n"
            if ctor not in file_data:
                raise RuntimeError("empty rpcdata constructor marker missing")
            file_data = file_data.replace(ctor, "")
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
                db_include = '#include "oi_db_json.h"\n'
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

    # 生成整行回调，字段表的元素类型只依赖行类型。
    def DbFieldEntry(self, member):
        codec = "Json" if member.db_json else "Value"
        if member.db_type:
            sql_type = '"%s"' % member.db_type
        elif member.db_json:
            sql_type = '"LONGTEXT"'
        else:
            sql_type = '::OI::Db::DefaultSqlType<%s>()' % QualType(member.GetType())
        return ('{"%s",\n'
                '                +[](const %s& stRow) -> ::OI::Db::Parameter\n'
                '                {\n'
                '                    return ::OI::Db::EncodeDb%s(stRow.%s);\n'
                '                },\n'
                '                +[](const ::OI::Db::Blob& stBlob, %s& stRow) -> bool\n'
                '                {\n'
                '                    return ::OI::Db::DecodeDb%s(stBlob, stRow.%s);\n'
                '                }, %s, %s}' %
                (member.db_name, self.class_name, codec, member.name,
                 self.class_name, codec, member.name, sql_type,
                 "true" if member.db_key else "false"))

    def GetDbMethods(self):
        members = [member for member in self.data.members if member.db_name]
        lines = []
        lines.append("    static constexpr size_t DbFieldCount = %d;" % len(members))
        lines.append("    static const std::array<::OI::Db::Field<%s>, %d>& DbFields()" % (self.class_name, len(members)))
        lines.append("    {")
        lines.append("        static const std::array<::OI::Db::Field<%s>, %d> stFields{{" % (self.class_name, len(members)))
        lines.append(",\n".join("            " + self.DbFieldEntry(member) for member in members) + "}};")
        lines.append("        return stFields;")
        lines.append("    }")
        lines.append("    template <typename M>")
        lines.append("    static const ::OI::Db::Field<%s>* DbFindField(M %s::*pMember)" % (self.class_name, self.class_name))
        lines.append("    {")
        for index, member in enumerate(members):
            lines.append("        if constexpr (std::is_same_v<M, %s>)" % QualType(member.GetType()))
            lines.append("        {")
            lines.append("            if (pMember == &%s::%s)" % (self.class_name, member.name))
            lines.append("            {")
            lines.append("                return &DbFields()[%d];" % index)
            lines.append("            }")
            lines.append("        }")
        lines.append("        return nullptr;")
        lines.append("    }")
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
        lines.append("        ::OI::Db::AppendRowParameters(*this, vParameters);")
        lines.append("    }")
        lines.append("    bool FromDbRow(const std::vector<std::string>& vColumns, const std::vector<std::optional<::OI::Db::Blob>>& vRow)")
        lines.append("    {")
        lines.append("        return ::OI::Db::FromRow(*this, vColumns, vRow);")
        lines.append("    }")
        lines.append("    ::OI::Db::ParameterMap ToDbMap() const")
        lines.append("    {")
        lines.append("        return ::OI::Db::ToParameterMap(*this);")
        lines.append("    }")
        lines.append("    bool FromDbMap(const ::OI::Db::CellMap& mCells)")
        lines.append("    {")
        lines.append("        return ::OI::Db::FromCellMap(*this, mCells);")
        lines.append("    }")
        return "\n".join(lines) + "\n"
