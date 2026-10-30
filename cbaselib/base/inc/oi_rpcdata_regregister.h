/* 由 proto 工具生成后拷进 base，不进 proto.xml。rpc 号只是 req/res 的映射。 */
#pragma once
#include "oi_rpc.h"
#include "oi_json.h"
#include <memory>
#include <string>

namespace OI { namespace RpcData {
class RegRegister : public ::OI::Rpc::RpcData
{
public:
    std::string name;
    int32_t id;
    std::string ip;
    std::string port;
    
    explicit RegRegister (){}
    RegRegister ( const std::string & l_name, const int32_t & l_id, const std::string & l_ip, const std::string & l_port )
    {
        name = l_name;
        id = l_id;
        ip = l_ip;
        port = l_port;
        
    }
    virtual ::OI::Base::BytesStream& serialize(::OI::Base::BytesStream& os) const
    {
        os.WritePb(0, name);
        os.WritePb(1, id);
        os.WritePb(2, ip);
        os.WritePb(3, port);
        return os;
    }
    virtual ::OI::Base::BytesStream& deserialize(::OI::Base::BytesStream& os)
    {
        while (os.RemainSize() > 0)
        {
            uint32_t field_seq = 0;
            uint32_t field_size = 0;
            os.ReadPbHeader(field_seq, field_size);
            switch (field_seq)
            {
            case 0: os.ReadPb(field_size, name); break;
            case 1: os.ReadPb(field_size, id); break;
            case 2: os.ReadPb(field_size, ip); break;
            case 3: os.ReadPb(field_size, port); break;
            default: os.SkipPb(field_size); break;
            }
        }
        return os;
    }
    bool FromJson(const Json::Value& in)
    {
        if (!in.isObject()) return false;
        if (in.isMember("name") && !::OI::Rpc::FromJsonValue(in["name"], name)) return false;
        if (in.isMember("id") && !::OI::Rpc::FromJsonValue(in["id"], id)) return false;
        if (in.isMember("ip") && !::OI::Rpc::FromJsonValue(in["ip"], ip)) return false;
        if (in.isMember("port") && !::OI::Rpc::FromJsonValue(in["port"], port)) return false;
        return true;
    }
    bool FromJson(const std::string& text)
    {
        Json::CharReaderBuilder builder;
        Json::Value root;
        std::string errs;
        const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        const char* begin = text.data();
        if (!reader->parse(begin, begin + text.size(), &root, &errs)) return false;
        return FromJson(root);
    }
    void ToJson(Json::Value& out) const
    {
        out = Json::Value(Json::objectValue);
        ::OI::Rpc::ToJsonValue(out["name"], name);
        ::OI::Rpc::ToJsonValue(out["id"], id);
        ::OI::Rpc::ToJsonValue(out["ip"], ip);
        ::OI::Rpc::ToJsonValue(out["port"], port);
    }
    std::string ToJson() const
    {
        Json::Value out;
        ToJson(out);
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        return Json::writeString(builder, out);
    }
};
}}
