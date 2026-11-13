#include "oi_log.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string>
#include "oi_log_client.h"
#include "oi_config.h"
#include "oi_base.h"
#include "oi_errno.h"
namespace OI
{
namespace Log
{
DiagnosticText::DiagnosticText(std::string_view sText) noexcept
{
	constexpr char vHex[] = "0123456789abcdef";
	size_t iUsed = 0;
	for (const auto& iCharacter : sText)
	{
		const auto iByte = static_cast<unsigned char>(iCharacter);
		const bool bEscape = iByte < 32 || iByte == 127 || iByte == '\\';
		const size_t iNeeded = bEscape ? 4 : 1;
		if (iUsed + iNeeded >= sizeof(m_vText))
		{
			m_bTruncated = true;
			break;
		}
		if (bEscape)
		{
			m_vText[iUsed++] = '\\';
			m_vText[iUsed++] = 'x';
			m_vText[iUsed++] = vHex[iByte >> 4];
			m_vText[iUsed++] = vHex[iByte & 15];
		}
		else
		{
			m_vText[iUsed++] = iCharacter;
		}
	}
	m_vText[iUsed] = 0;
}

static std::string sLogTitle;
static bool _logclient_init = false;
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

int32_t Init()
{
	const int32_t iReturnCode = LogClient::GetInstance().Init();
	if (iReturnCode != OI_OK)
	{
		return iReturnCode;
	}
	LogClient::GetInstance().StartClient();
	InitTitle();
	_logclient_init = true;
	return OI_OK;
}

void InitLocal(LocalSink fnSink)
{
	InitTitle();
	_local_sink = fnSink;
}

int32_t Log(LOG_LEVEL iLevel, const char* pFormat, ...)
{
	// 先格式化到栈上，放得下就只格式化一次；放不下再按实际长度格式化进 tmp。
	char vStack[2048];
	va_list vVl;
	va_start(vVl, pFormat);
	va_list vAgain;
	va_copy(vAgain, vVl);
	int iNeed = vsnprintf(vStack, sizeof(vStack), pFormat, vVl);
	va_end(vVl);
	if (iNeed < 0)
	{
		va_end(vAgain);
		return OI_ERR_ENCODE;
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
		vsnprintf(stTmp.End(), (size_t)iNeed + 1, pFormat, vAgain);
		stTmp.Commit((size_t)iNeed);
	}
	va_end(vAgain);
	stTmp.Append("\n", 1);

	fwrite(stTmp.Data(), 1, stTmp.Size(), stdout);

	if (_local_sink)
	{
		return _local_sink(iLevel, stTmp.Data(), stTmp.Size());
	}
	if (_logclient_init)
	{
		return LogClient::GetInstance().SendLog((int)iLevel, stTmp.Data(), stTmp.Size());
	}

	return OI_ERR_CONNECT;
}
}
}
