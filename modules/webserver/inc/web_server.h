#pragma once
#include "oi_singleton.h"
#include <cstdint>

class WebServer
{
	DECLARE_SINGLETON(WebServer)
	WebServer() = default;

  public:
	int32_t Run();
};
