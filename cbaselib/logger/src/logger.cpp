#include "logger.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string>
#include "log_client.h"
#include "config.h"
#include "base_header.h"
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
		va_list vl;
		va_start(vl, format);
		va_list measure;
		va_copy(measure, vl);
		int need = vsnprintf(NULL, 0, format, measure);
		va_end(measure);
		if (need < 0)
		{
			va_end(vl);
			return false;
		}
		std::string body((size_t)need + 1, '\0');
		int wrote = vsnprintf(&body[0], body.size(), format, vl);
		va_end(vl);
		if (wrote < 0)
		{
			return false;
		}
		Bytes tmp;
		tmp.Reserve(_log_title.length() + (size_t)wrote + 2);
		tmp.Append(_log_title.c_str(), _log_title.length());
		tmp.Append(body.data(), (size_t)wrote);
		tmp.Append("\n", 1);

		if (lv == LOG_LEVEL::LOG_TRACE)
		{
			tmp.Append("", 1);
			printf("%s", tmp.Begin());
		}

		if (_logclient_init)
		{
			return LogClient::GetInstance().SendLog((int)lv, tmp.Data(), tmp.Size());
		}

		return false;
	}
}
