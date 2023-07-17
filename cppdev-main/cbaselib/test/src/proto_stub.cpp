#include "header.h" 
#include "testdata.hrp" 
#include "testrpc.hrp" 
#include "testprotocol.hpp" 
#include "testprotocol2.hpp" 

void s_RegisterStub() FUNCTION_RUN_BEFORE_MAIN;
void s_RegisterStub()
{
	ProtocolStubs::GetInstance().RegisterStub(new TestData ( new TestDataReq(), new TestDataRes()));
	ProtocolStubs::GetInstance().RegisterStub(new TestRpc ( new TestReq(), new TestRes()));
	ProtocolStubs::GetInstance().RegisterStub(new TestProtocol ( ));
	ProtocolStubs::GetInstance().RegisterStub(new TestProtocol2 ( ));

}
