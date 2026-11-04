#include <assert.h>
#include "oi_net_session.h"

class LogServerSession : public ::OI::Net::NetSession
{
	bool m_bWaiting = false;
	bool m_bShutdown = false;

  public:
	LogServerSession(::OI::Net::SessionManager* pMgrParam)
	    : NetSession(pMgrParam)
	{
	}
	virtual ~LogServerSession()
	{
	}
	void OnDataIn();
	void WaitForWrite();
	void ResumeWrite(bool bShutdownParam);
	void OnDataOut()
	{
		assert(false && u8"logserver不需要回信");
	}
};
