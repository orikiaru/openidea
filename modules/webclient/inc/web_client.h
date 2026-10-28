#pragma once
#include "singleton.h"

class WebClient
{
	DECLARE_SINGLETON(WebClient)
	WebClient() = default;
public:
	int Run();
};
