#include "oi_config.h"
#include <iostream>

namespace OI::Base
{
SingletonUpdates& SingletonUpdates::Instance()
{
	static SingletonUpdates stUpdates;
	return stUpdates;
}
}

bool ReadMissing()
{
	int iValue = 17;
	TryGetConfigAndReturnFalse("missing", iValue);
	return true;
}

bool ReadExisting()
{
	int iValue = 0;
	TryGetConfigAndReturnFalse("value", iValue);
	return iValue == 42;
}

#ifdef OI_CONFIG_TEST_INVALID_RETURN
int ReadMissingAsInt()
{
	int iValue = 17;
	TryGetConfigAndReturnFalse("missing", iValue);
	return 1;
}
#endif

#define CHECK(CONDITION)                                          \
	do                                                            \
	{                                                             \
		if (!(CONDITION))                                         \
		{                                                         \
			std::cerr << "check failed line " << __LINE__ << ": " \
			          << #CONDITION << std::endl;                   \
			return 1;                                             \
		}                                                         \
	} while (false)

int main(int iArgCount, char** pArguments)
{
	CHECK(iArgCount == 3);
	CHECK(ParseConfig(pArguments[1]) == OI_OK);
	int iValue = 0;
	GetConfigAndAssert("value", iValue);
	CHECK(iValue == 42);
	CHECK(!ReadMissing());
	CHECK(ReadExisting());
	int iOutput = 17;
	CHECK(!TryGetConfig("missing", iOutput));
	CHECK(iOutput == 17);
	CHECK(!TryGetConfig("text", iOutput));
	CHECK(iOutput == 17);
	std::vector<int64_t> vOutput = {17, 19};
	const std::vector<int64_t> vOriginal = vOutput;
	CHECK(!OI::Base::Config::GlobalConfig().GetArrayAs<int64_t>("missing", vOutput));
	CHECK(vOutput == vOriginal);
	CHECK(!OI::Base::Config::GlobalConfig().GetArrayAs<int64_t>("bad_array", vOutput));
	CHECK(vOutput == vOriginal);
	CHECK(OI::Base::Config::GlobalConfig().GetArrayAs<int64_t>("good_array", vOutput));
	CHECK((vOutput == std::vector<int64_t>{1, 2}));
	CHECK(ParseConfig(pArguments[2]) == OI_ERR_INVALID_ARGUMENT);
	iValue = 0;
	CHECK(TryGetConfig("value", iValue));
	CHECK(iValue == 42);
	return 0;
}
