#pragma once
#include "oi_coro.h"
#include "oi_singleton.h"

class DbMgr
{
	DECLARE_SINGLETON(DbMgr)
	DbMgr() = default;
	bool m_bEnabled = false;
	bool m_bMysql = false;
	OI::Coro::Task Run();

  public:
	void Load();
	void Start();
};
