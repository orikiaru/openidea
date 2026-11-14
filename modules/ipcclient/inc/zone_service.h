#pragma once
#include "oi_rpc_session_manager.h"
#include <string>
#include <utility>

class ZoneService final : public ::OI::Rpc::ProtoClientManager
{
  public:
	explicit ZoneService(std::string sName)
	{
		sServiceName = std::move(sName);
	}
	const std::string GetIDString() override
	{
		return "ZoneService";
	}
	void OnClientConnect(::OI::Net::Session*) override
	{
	}
	void OnClientDisconnect(const ::OI::Net::Session*) override
	{
	}
};
