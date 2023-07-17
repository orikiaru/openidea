#include "log_protocol.h"
#include "log_parser.h"

class LogServerSession : public NetSession
{
	LogProtocolParser _parser;
public:
	LogServerSession(SessionManager* pMgr) :NetSession(pMgr) {}
	virtual ~LogServerSession() {}
	virtual void SendProtocol(const Protocol* proto)
	{
		assert(false && u8"logserver不需要回信");
	}
	void OnDataIn();
	void OnDataOut() { assert(false && u8"logserver不需要回信"); }
};