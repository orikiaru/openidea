/* 由 proto 工具生成后拷进 base，不进 proto.xml。rpc 号只是 req/res 的映射。 */
#pragma once
#include "oi_rpc.h"
#include "oi_json.h"
#include <memory>
#include <string>

namespace OI { namespace RpcData {
class RegHeartbeat : public ::OI::Rpc::RpcData
{
public:
    std::string name;
    
    explicit RegHeartbeat (){}
    RegHeartbeat ( const std::string & l_name )
    {
        name = l_name;
        
    }
    virtual ::OI::Base::BytesStream& serialize(::OI::Base::BytesStream& os) const
    {
        os.WritePb(0, name);
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
            default: os.SkipPb(field_size); break;
            }
        }
        return os;
    }
    bool FromJson(const Json::Value& in)
    {
        if (!in.isObject()) return false;
        if (in.isMember("name") && !::OI::Rpc::FromJsonValue(in["name"], name)) return false;
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
    }
    std::string ToJson() const
    {
        Json::Value out;
        ToJson(out);
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        return Json::writeString(builder, out);
    }
    virtual ::OI::Rpc::RpcData* Clone() { return new RegHeartbeat(*this); }
};
}}
