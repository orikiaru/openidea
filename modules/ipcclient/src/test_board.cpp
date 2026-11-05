#include "test_board.h"
#include "oi_thread.h"

TestBoard::TestBoard()
    : m_iLeft(0), m_iCode(0)
{
}

void TestBoard::Arm()
{
	m_iLeft.fetch_add(1);
}

void TestBoard::Done(int iCode)
{
	if (iCode != 0)
	{
		m_iCode.store(iCode);
	}
	if (m_iLeft.fetch_sub(1) == 1)
	{
		::OI::Thread::ThreadPool::GetInstance().RequestStop(m_iCode.load());
	}
}
