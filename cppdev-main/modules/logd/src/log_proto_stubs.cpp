#include "log_proto_stubs.h"

void s_log_RegisterStub() FUNCTION_RUN_BEFORE_MAIN;
void s_log_RegisterStub()
{
	LogProtoStubs::GetInstance().RegisterStub(new RemoteLog());
}
