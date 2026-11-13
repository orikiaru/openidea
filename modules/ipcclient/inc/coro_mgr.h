#pragma once
#include "oi_coro.h"
#include "oi_lock.h"
#include "oi_singleton.h"
#include <atomic>
#include <cstdint>
#include <vector>

class CoroMgr
{
	DECLARE_SINGLETON(CoroMgr)
	CoroMgr();
	bool m_bEnabled;
	int32_t m_iLive;
	int32_t m_iChurn;
	std::vector<std::coroutine_handle<>> m_vHeld;
	OI::Thread::Mutex m_stHeldMutex;
	std::atomic<int32_t> m_iWakeDone;
	std::atomic<int32_t> m_iChurnDone;

  public:
	void Load();
	void Start();
	OI::Coro::Task Run();
	void Hold(std::coroutine_handle<> stHandle);
	int32_t HeldCount();
	std::vector<std::coroutine_handle<>> CopyHeld();
	void NoteWake();
	void NoteChurn();
	int32_t WakeDone() const;
	int32_t ChurnDone() const;
};
