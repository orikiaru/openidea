#include "log_protocol.h"
#include <assert.h>
#include "log_server.h"
#include "log_manager.h"

void RemoteLog::Handle(SessionManager* mgr, sid_t sid)
{
	if (&LogServer::GetInstance() != mgr)
	{
		assert(false);
		return;
	}
	if (_lv < 0 || _lv >= LOG_LEVEL::LOG_LEVEL_COUNT)
	{
		LOG_TRACE("RemoteLog::Handle error lv=%d", _lv);
		return;
	}
	LogManager::GetInstance().WriteLog((LOG_LEVEL)_lv, _log_content);
}