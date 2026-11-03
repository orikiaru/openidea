#include "oi_log.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string>
#include "oi_log_client.h"
#include "oi_config.h"
#include "oi_base.h"
namespace OI { namespace Log
{
	static std::string sLogTitle;
	static bool	_logclient_init = false;
	static LocalSink _local_sink = NULL;

	static void InitTitle()
	{
		int iServiceIdLocal = 1;
		std::string sServiceNameLocal = "UnKnownService";
		TryGetConfigNoReturn("Service.id", iServiceIdLocal);
		TryGetConfigNoReturn("Service.name", sServiceNameLocal);
		char vBuffer[1024];
		int iMaxLength = sizeof(vBuffer) - 1;
		snprintf(vBuffer, iMaxLength, "%s-%d:", sServiceNameLocal.c_str(), iServiceIdLocal);
		sLogTitle = vBuffer;
	}

	bool Init()
	{
		bool bReturnCode = LogClient::GetInstance().Init();
		if (!bReturnCode)
		{
			return false;
		}
		LogClient::GetInstance().StartClient();
		InitTitle();
		_logclient_init = true;
		return true;
	}

	void InitLocal(LocalSink fnSinkParam)
	{
		InitTitle();
		_local_sink = fnSinkParam;
	}

	bool Log(LOG_LEVEL iLevelParam, const char* pFormatParam, ...)
	{
		// 先格式化到栈上，放得下就只格式化一次；放不下再按实际长度格式化进 tmp。
		char vStack[2048];
		va_list vVl;
		va_start(vVl, pFormatParam);
		va_list vAgain;
		va_copy(vAgain, vVl);
		int iNeed = vsnprintf(vStack, sizeof(vStack), pFormatParam, vVl);
		va_end(vVl);
		if (iNeed < 0)
		{
			va_end(vAgain);
			return false;
		}
		::OI::Base::Bytes stTmp;
		stTmp.Reserve(sLogTitle.length() + (size_t)iNeed + 2);
		stTmp.Append(sLogTitle.c_str(), sLogTitle.length());
		if ((size_t)iNeed < sizeof(vStack))
		{
			stTmp.Append(vStack, (size_t)iNeed);
		}
		else
		{
			vsnprintf(stTmp.End(), (size_t)iNeed + 1, pFormatParam, vAgain);
			stTmp.Commit((size_t)iNeed);
		}
		va_end(vAgain);
		stTmp.Append("\n", 1);

		fwrite(stTmp.Data(), 1, stTmp.Size(), stdout);

		if (_local_sink)
		{
			return _local_sink(iLevelParam, stTmp.Data(), stTmp.Size());
		}
		if (_logclient_init)
		{
			return LogClient::GetInstance().SendLog((int)iLevelParam, stTmp.Data(), stTmp.Size());
		}

		return false;
	}
}}
