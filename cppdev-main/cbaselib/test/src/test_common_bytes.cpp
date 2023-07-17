#include <stdio.h>
#include <stdlib.h>
#include "test_common_bytes.h"
#include "bytes.h"
#include "base_timer_helper.h"

void TestCommonBytes::DoPerformanceTest()
{
	bool test_bytes = false;
	int test_bytes_count = 1000000;
	TryGetConfigNoReturn("Test.test_bytes", test_bytes);
	TryGetConfigNoReturn("Test.test_bytes_count", test_bytes_count);
	if (!test_bytes)
	{
		return;
	}
	timeval old, now;
	gettimeofday(&old,0);
	Bytes b;
	for (int i = 0; i < test_bytes_count; ++i)
	{
		Bytes bytes = b;
		bytes.Append("123456789", 9);
		//printf("%.*s\n", (int)bytes.Size(), bytes.Begin());
		bytes.Append("123456789", 9);
		//printf("%.*s\n", (int)bytes.Size(), bytes.Begin());
		bytes.Erase(0, 8);
		//printf("%.*s\n", (int)bytes.Size(), bytes.Begin());
		bytes.Erase(0, 1);
		//printf("%.*s\n", (int)bytes.Size(), bytes.Begin());
		bytes.Insert(5, "xxxx", 4);
		//printf("%.*s\n", (int)bytes.Size(), bytes.Begin());
		bytes.Erase(5, 4);
		//printf("%.*s\n", (int)bytes.Size(), bytes.Begin());
	}
	gettimeofday(&now, 0);

	printf("TestCommonBytes::DoPerformanceTest cost time=%lf\n", (((now.tv_sec - old.tv_sec)* 1e6) + now.tv_usec - old.tv_usec)/1e6);
}
