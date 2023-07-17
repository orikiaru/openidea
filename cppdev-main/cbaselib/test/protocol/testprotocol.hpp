#pragma once
#include "net_header.h"
class TestProtocol : public Protocol 
{
public:
      #include "testprotocol" 
      virtual void Handle( SessionManager* mgr, sid_t sid)
      {
          printf("TestProtocol::Handle %d,%d,%d,%ld,%f,%lf,%s\n", 
              this->_cid, this->_sid, this->_iid, this->_lid, this->_fscore, this->_dscore, this->_msg.c_str());
      }
};
