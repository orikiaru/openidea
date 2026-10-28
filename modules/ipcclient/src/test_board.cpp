#include "test_board.h"
#include "oi_thread.h"

TestBoard::TestBoard() : _left(0), _code(0)
{
}

void TestBoard::Arm()
{
	_left.fetch_add(1);
}

void TestBoard::Done(int code)
{
	if (code != 0)
	{
		_code.store(code);
	}
	if (_left.fetch_sub(1) == 1)
	{
		::OI::Thread::ThreadPool::GetInstance().RequestStop(_code.load());
	}
}
