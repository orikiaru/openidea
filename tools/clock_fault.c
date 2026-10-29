#define _GNU_SOURCE
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* 只改变被测进程的墙上时间，不修改宿主或 Redis 的时钟。 */
static struct timespec started;
__attribute__((constructor)) static void init_clock_fault(void)
{
	clock_gettime(CLOCK_MONOTONIC, &started);
}

int gettimeofday(struct timeval* tv, void* tz)
{
	int rc = (int)syscall(SYS_gettimeofday, tv, tz);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (rc == 0 && now.tv_sec - started.tv_sec >= 2)
	{
		const char* shift = getenv("OI_TEST_CLOCK_SHIFT");
		if (shift) tv->tv_sec += strtol(shift, NULL, 10);
	}
	return rc;
}
