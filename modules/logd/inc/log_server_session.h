#include <assert.h>
#include "oi_net_session.h"

class LogServerSession : public NetSession
{
public:
	LogServerSession(SessionManager* pMgr) :NetSession(pMgr) {}
	virtual ~LogServerSession() {}
	virtual void SendProtocol(const Protocol* proto)
	{
		(void)proto;
		assert(false && u8"logserver不需要回信");
	}
	void OnDataIn();
	void OnDataOut() { assert(false && u8"logserver不需要回信"); }
};
