#include "oi_log.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string>
#include "oi_log_client.h"
#include "oi_config.h"
#include "oi_base.h"
namespace Log
{
	static std::string _log_title;
	static bool	_logclient_init = false;
	bool Init()
	{
		bool ret = LogClient::GetInstance().Init();
		if (!ret)
		{
			return false;
		}
		LogClient::GetInstance().StartClient();

		int service_id = 1;
		std::string service_name = "UnKnownService";
		TryGetConfigNoReturn("Service.id", service_id);
		TryGetConfigNoReturn("Service.name", service_name);
		char buff[1024];
		int max_len = sizeof(buff) - 1;
		snprintf(buff, max_len, "%s-%d:", service_name.c_str(), service_id);
		_log_title = buff;
		_logclient_init = true;
		return true;
	}

	bool Log(LOG_LEVEL lv, const char* format, ...)
	{
		// 先格式化到栈上，放得下就只格式化一次；放不下再按实际长度格式化进 tmp。
		char stack[2048];
		va_list vl;
		va_start(vl, format);
		va_list again;
		va_copy(again, vl);
		int need = vsnprintf(stack, sizeof(stack), format, vl);
		va_end(vl);
		if (need < 0)
		{
			va_end(again);
			return false;
		}
		OI::Base::Bytes tmp;
		tmp.Reserve(_log_title.length() + (size_t)need + 2);
		tmp.Append(_log_title.c_str(), _log_title.length());
		if ((size_t)need < sizeof(stack))
		{
			tmp.Append(stack, (size_t)need);
		}
		else
		{
			vsnprintf(tmp.End(), (size_t)need + 1, format, again);
			tmp.Commit((size_t)need);
		}
		va_end(again);
		tmp.Append("\n", 1);

		if (lv == LOG_LEVEL::LOG_TRACE)
		{
			fwrite(tmp.Data(), 1, tmp.Size(), stdout);
		}

		if (_logclient_init)
		{
			return LogClient::GetInstance().SendLog((int)lv, tmp.Data(), tmp.Size());
		}

		return false;
	}
}
