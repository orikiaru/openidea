#include "proto_session_manager.h"
#include "timer_task.h"
#include "base_header.h"

bool ProtoClientManager::SendProtocol(const Protocol* p)
{
	if (_conn && _ssid != INVALID_SID)
	{
		return Send(_ssid, p);
	}
	return false;
}

void ProtoClientManager::Reconnect(int millisecond)
{
	class ReconnTask : public TimerTask
	{
	public:
		ProtoClientManager* p_mgr;
		ReconnTask(ProtoClientManager* pMgr) :p_mgr(pMgr) {}
		virtual void Handle()
		{
			p_mgr->StartClient();
		}
	};

	BaseTimerManager::GetInstance()->AddTimer(new ReconnTask(this), millisecond);
}
