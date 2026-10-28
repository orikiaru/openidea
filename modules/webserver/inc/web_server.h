#pragma once
#include "singleton.h"

class WebServer
{
	DECLARE_SINGLETON(WebServer)
	WebServer() = default;
public:
	bool Run();
};
