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

	���ƫ��1tick,����ͳ��tickƫ������ʱ������

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
		data += offset;	 //��ֹ��Խ��
	}
	if (data > 100000000)
	{
		data = 100000000 - 100000000 % offset;	//��ֹ��Խ��
	}

	if (std::is_same<DT, DT_DAY>::value)
	{
		//dataΪһ��dt�Ļ�����ֵ s�������һ��, min�������һ���ӵ���ʼ
		for (int i = 1; i < offset; i += rand()%100)		//��ƫ����
		{
			for (int j = 1; j < offset; j += rand() % 100)	//��ƫ����
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
		//dataΪһ��dt�Ļ�����ֵ s�������һ��, min�������һ���ӵ���ʼ
		for (int i = 1; i < offset; ++i)		//��ƫ����
		{
			for (int j = 1; j < offset; ++j)	//��ƫ����
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
	//ȥ��ʱ��Ӱ��,��������ʹ��!!!!
	setenv("TZ", "UTC", 1);
	tzset();

	int data = rand() / 1;

	TestHelperDelta<DT_SECOND>(data, 1);

	TestHelperDelta<DT_MINUTE>(data - data % 60, 60);

	TestHelperDelta<DT_HOUR>(data - data % 3600, 3600);

	TestHelperDelta<DT_DAY>(data - data % 86400, 86400);	//0�ǰ˵���,��Ϊ�Ƕ�����,���Ի�µ�

	//���±Ƚϼ� �Ͳ�����

}