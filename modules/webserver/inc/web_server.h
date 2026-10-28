#pragma once
#include "oi_singleton.h"

class WebServer
{
	DECLARE_SINGLETON(WebServer)
	WebServer() = default;
public:
	bool Run();
};
