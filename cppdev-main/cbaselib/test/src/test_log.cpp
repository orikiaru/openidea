#include "test_log.h"
#include "logger.h"
#include "base_header.h"
#include <stdint.h>

char hex_buff[2];

static char GetHex(uint8_t data)
{
	if (data >= 0 && data <= 9)
	{
		return '0' + data;
	}
	else
	{
		return 'a' + data - 10;
	}
}

static char* Data2Hex(uint8_t data)
{
	uint8_t head = data / 16;
	uint8_t tail = data % 16;
	hex_buff[1] = GetHex(tail);
	hex_buff[0] = GetHex(head);
	return hex_buff;
}

class LogTestTask : public TimerTask
{
	int _test_count;
	int _test_len;
public:
	LogTestTask(int count, int len) : _test_count(count), _test_len(len) {}
	virtual void Handle()
	{
		assert(_test_len > 1);
		char buff[_test_len];
		int offset = 0;
		for (int i = 0; offset + 2 < _test_len - 1; ++i)
		{
			uint8_t data = rand();
			memcpy(buff + offset, Data2Hex(data), 2);
			offset += 2;
		}
		assert(offset < _test_len - 1);
		buff[offset] = 0;
		for (int i = 0; i < _test_count; ++i)
		{
			LOG_DEBUG(buff);
		}
		BaseTimerManager::GetInstance()->AddTimer(new LogTestTask(_test_count, _test_len), 1000);
	}

};

namespace TestLog
{
	void DoPerformanceTest()
	{
		bool test_log = false;
		TryGetConfigNoReturn("Test.test_log", test_log);
		if (test_log)
		{
			int test_log_count = 10000;
			int test_log_len = 200;
			TryGetConfigNoReturn("Test.test_log_count", test_log_count);
			TryGetConfigNoReturn("Test.test_log_len", test_log_len);
			BaseTimerManager::GetInstance()->AddTimer(new LogTestTask(test_log_count, test_log_len), 1000);
		}
	}
}

