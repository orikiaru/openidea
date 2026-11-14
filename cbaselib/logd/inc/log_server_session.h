#include <assert.h>
#include "oi_net_session.h"

class LogServerSession : public ::OI::Net::NetSession
{
	bool m_bWaiting = false;
	bool m_bShutdown = false;

  public:
	LogServerSession(::OI::Net::SessionManager* pMgr)
	    : NetSession(pMgr)
	{
	}
	virtual ~LogServerSession()
	{
	}
	void OnDataIn() override;
	void WaitForWrite();
	void ResumeWrite(bool bShutdown);
	void OnDataOut() override
	{
		assert(false && u8"logserver不需要回信");
	}
};
