#pragma once
#include "net_header.h"
class TestProtocol2 : public Protocol 
{
public:
      #include "testprotocol2" 
      virtual void Handle( SessionManager* mgr, sid_t sid)
      {
          printf("TestProtocol2::Handle %d\n", this->test_data);
      }
};
