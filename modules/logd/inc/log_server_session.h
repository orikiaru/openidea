#include <assert.h>
#include "oi_net_session.h"

class LogServerSession : public ::OI::Net::NetSession
{
public:
	LogServerSession(::OI::Net::SessionManager* pMgr) :NetSession(pMgr) {}
	virtual ~LogServerSession() {}
	void OnDataIn();
	void OnDataOut() { assert(false && u8"logserver不需要回信"); }
};
