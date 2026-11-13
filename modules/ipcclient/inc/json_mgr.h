#pragma once
#include "oi_singleton.h"

class JsonMgr
{
	DECLARE_SINGLETON(JsonMgr)
	JsonMgr() = default;
	// 这一宽度整数的 JSON 上下界。越界、小数和字符串都不能改已有值。
	template <typename T>
	int CheckJsonInteger();
	// 各宽度整数，再加上非法键、越界元素和坏对象。出现的字段才更新。
	int CheckJsonDecoding();

  public:
	// 各宽度整数的 JSON 上下界。非法键、越界元素和坏对象解码失败，并且不改已有值。出现的字段才更新。
	int Run();
};
