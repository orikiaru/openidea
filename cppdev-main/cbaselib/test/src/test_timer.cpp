#include "test_timer.h"
#include "base_timer_manager.h"
#include "base_timer_helper.h"
#include "config.h"

void TestHelper();

TestTimer::TestTimer(int offset)
{
	_offset = offset;
	_before_tick = BaseTimerManager::GetInstance()->GetCurTick();
}

bool TestTimer::OnTimer()
{
	//assert(_before_tick == BaseTimerManager::GetInstance()->GetCurTick() - _offset);
	int offset = rand() % 100 + 1;
	TestTimer* pTimer = new TestTimer(offset);
	BaseTimerManager::GetInstance()->AddTimer(pTimer, offset * 50);
	printf("TestTimer::OnTimer %s\n", GenLog().c_str());
	delete this;
	return false;
}

std::string TestTimer::GenLog()
{
	char buff[1024];
	sprintf(buff, "before_tick=%8ld, cur_tick=%8ld, offset=%4d, real_offset=%6ld",
		_before_tick, BaseTimerManager::GetInstance()->GetCurTick(), _offset,
		BaseTimerManager::GetInstance()->GetCurTick() - _offset - _before_tick);
	return buff;
}

void TestTimer::DoPerformanceTest()
{
	/*
	grep -E "real_offset=     [1-9]" 1 | wc -l
	35782
	grep -E "real_offset=     [1]" 1 | wc -l
	35782
	grep -E "real_offset=     [0]" 1 | wc -l
	3331659

	最多偏离1tick,仍需统计tick偏离物理时间的情况

	*/

	bool test_timer = false;
	TryGetConfigNoReturn("Test.test_timer", test_timer);
	if (test_timer)
	{
		int test_count = 100;
		TryGetConfigNoReturn("Test.test_timer_count", test_count);
		int test_range = 1000;
		TryGetConfigNoReturn("Test.test_timer_range", test_range);
		for (int i = 0; i < test_count; ++i)
		{
			int offset = rand() % test_range + 1;
			TestTimer* pTimer = new TestTimer(offset);
			BaseTimerManager::GetInstance()->AddTimer(pTimer, offset * 50);
		}

		TestHelper();
	}


}

template<typename DT>
static void TestHelperDelta(int data, int offset)
{
	if (data < offset)
	{
		data += offset;	 //防止减越界
	}
	if (data > 100000000)
	{
		data = 100000000 - 100000000 % offset;	//防止加越界
	}

	if (std::is_same<DT, DT_DAY>::value)
	{
		//data为一种dt的基础数值 s就是随便一秒, min就是随便一分钟的起始
		for (int i = 1; i < offset; i += rand()%100)		//左偏移量
		{
			for (int j = 1; j < offset; j += rand() % 100)	//右偏移量
			{
				assert(true == TimerHelper::IsSame<DT>(data + j, data + i));
				assert(false == TimerHelper::IsCross<DT>(data + j, data + i));

				assert(false == TimerHelper::IsSame<DT>(data - i, data + j));
				assert(true == TimerHelper::IsCross<DT>(data - i, data + j));
				assert(false == TimerHelper::IsCross<DT>(data + j, data - i));
			}
		}
	}
	else
	{
		//data为一种dt的基础数值 s就是随便一秒, min就是随便一分钟的起始
		for (int i = 1; i < offset; ++i)		//左偏移量
		{
			for (int j = 1; j < offset; ++j)	//右偏移量
			{
				assert(true == TimerHelper::IsSame<DT>(data + j, data + i));
				assert(false == TimerHelper::IsCross<DT>(data + j, data + i));

				assert(false == TimerHelper::IsSame<DT>(data - i, data + j));
				assert(true == TimerHelper::IsCross<DT>(data - i, data + j));
				assert(false == TimerHelper::IsCross<DT>(data + j, data - i));
			}
		}
	}
}

void TestHelper()
{
	//去除时区影响,仅供测试使用!!!!
	setenv("TZ", "UTC", 1);
	tzset();

	int data = rand() / 1;

	TestHelperDelta<DT_SECOND>(data, 1);

	TestHelperDelta<DT_MINUTE>(data - data % 60, 60);

	TestHelperDelta<DT_HOUR>(data - data % 3600, 3600);

	TestHelperDelta<DT_DAY>(data - data % 86400, 86400);	//0是八点起,因为是东八区,所以会嘎掉

	//年月比较简单 就不测了

}