/* 由 proto 工具生成后拷进 base，不进 proto.xml。rpc 号只是 req/res 的映射。 */
#pragma once
#include "oi_rpc.h"
#include "oi_json.h"
#include <memory>
#include <string>

namespace OI
{
namespace RpcData
{
class RegInstance : public ::OI::Rpc::RpcData
{
  public:
	std::string sName{};
	int32_t iId{};
	std::string sIp{};
	std::string sPort{};

	explicit RegInstance()
	{
	}
	RegInstance(const std::string& l_name, const int32_t& l_id, const std::string& l_ip, const std::string& l_port)
	{
		sName = l_name;
		iId = l_id;
		sIp = l_ip;
		sPort = l_port;
	}
	virtual ::OI::Base::BytesStream& serialize(::OI::Base::BytesStream& os) const
	{
		os.WritePb(0, sName);
		os.WritePb(1, iId);
		os.WritePb(2, sIp);
		os.WritePb(3, sPort);
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
			case 0:
				os.ReadPb(field_size, sName);
				break;
			case 1:
				os.ReadPb(field_size, iId);
				break;
			case 2:
				os.ReadPb(field_size, sIp);
				break;
			case 3:
				os.ReadPb(field_size, sPort);
				break;
			default:
				os.SkipPb(field_size);
				break;
			}
		}
		return os;
	}
	bool FromJson(const Json::Value& in)
	{
		if (!in.isObject())
		{
			return false;
		}
		if (in.isMember("name") && !::OI::Rpc::FromJsonValue(in["name"], sName))
		{
			return false;
		}
		if (in.isMember("id") && !::OI::Rpc::FromJsonValue(in["id"], iId))
		{
			return false;
		}
		if (in.isMember("ip") && !::OI::Rpc::FromJsonValue(in["ip"], sIp))
		{
			return false;
		}
		if (in.isMember("port") && !::OI::Rpc::FromJsonValue(in["port"], sPort))
		{
			return false;
		}
		return true;
	}
	bool FromJson(const std::string& text)
	{
		Json::CharReaderBuilder builder;
		Json::Value root;
		std::string errs;
		const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
		const char* begin = text.data();
		if (!reader->parse(begin, begin + text.size(), &root, &errs))
		{
			return false;
		}
		return FromJson(root);
	}
	void ToJson(Json::Value& out) const
	{
		out = Json::Value(Json::objectValue);
		::OI::Rpc::ToJsonValue(out["name"], sName);
		::OI::Rpc::ToJsonValue(out["id"], iId);
		::OI::Rpc::ToJsonValue(out["ip"], sIp);
		::OI::Rpc::ToJsonValue(out["port"], sPort);
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
}
}
