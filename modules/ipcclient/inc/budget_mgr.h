#pragma once
#include "oi_singleton.h"

class BudgetMgr
{
	DECLARE_SINGLETON(BudgetMgr)
	BudgetMgr() = default;
	// 发送额度跨会话缓冲连续计数，真正写出才恢复。关闭后发送回 CONNECT。
	int CheckSendBudget();

  public:
	// 发送额度跨会话缓冲连续计数，真正写出才恢复。关闭后发送回 CONNECT。
	int Run();
};
