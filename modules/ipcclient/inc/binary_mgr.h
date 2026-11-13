#pragma once
#include "oi_singleton.h"

class BinaryMgr
{
	DECLARE_SINGLETON(BinaryMgr)
	BinaryMgr() = default;
	// 布尔的二进制编码只认 0 和 1，其余字节拒绝。
	int CheckBinaryBool();

  public:
	// 布尔的二进制编码只认 0 和 1，其余字节拒绝。
	int Run();
};
