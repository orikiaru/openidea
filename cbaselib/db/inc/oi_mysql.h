#pragma once
#include "oi_db.h"

namespace OI::Mysql
{
struct Config
{
	std::string sHost = "127.0.0.1";
	std::string sUser;
	std::string sPassword;
	std::string sDatabase;
	// When set, TLS is required and the server identity is verified against this CA.
	std::string sTlsCaFile;
	uint32_t iPort = 3306;
	uint32_t iConnectTimeoutSeconds = 10;
	uint32_t iReadTimeoutSeconds = 30;
	uint32_t iWriteTimeoutSeconds = 30;
	size_t iMaxResultBytes = 16 * 1024 * 1024;
	size_t iMaxRows = 100000;
};

#ifdef OI_ENABLE_MYSQL
std::shared_ptr<OI::Db::Driver> MakeDriver(Config stConfig);
#endif
using Client = OI::Db::Client;
}
