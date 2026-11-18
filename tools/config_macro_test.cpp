#include "oi_config.h"

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

#ifdef OI_CONFIG_TEST_INVALID_RETURN
int ReadMissingAsInt()
{
	int iValue = 17;
	TryGetConfigAndReturnFalse("missing", iValue);
	return 1;
}
#endif

int main(int iArgCount, char** pArguments)
{
	if (iArgCount != 2 || ParseConfig(pArguments[1]) != OI_OK)
	{
		return 1;
	}
	int iValue = 0;
	GetConfigAndAssert("value", iValue);
	if (iValue != 42 || ReadMissing())
	{
		return 2;
	}
	return 0;
}
