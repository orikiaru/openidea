// Test-only LD_PRELOAD fixture: slow regular-file writes in logd's writer threads.
// It is never linked into an application and does not delay poll or stdout.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

static pthread_once_t stOnce = PTHREAD_ONCE_INIT;
static size_t (*fnWrite)(const void*, size_t, size_t, FILE*);

static void ResolveWrite(void)
{
    fnWrite = (size_t (*)(const void*, size_t, size_t, FILE*))dlsym(RTLD_NEXT, "fwrite");
}

size_t fwrite(const void* pDataParam, size_t iSizeParam, size_t iCountParam, FILE* pFileParam)
{
    pthread_once(&stOnce, ResolveWrite);
    const int iDescriptor = fileno(pFileParam);
    struct stat stInfo;
    if (iDescriptor > 2 && fstat(iDescriptor, &stInfo) == 0 && S_ISREG(stInfo.st_mode))
    {
        struct timespec stDelay = {0, 5000000};
        while (nanosleep(&stDelay, &stDelay) != 0)
        {
        }
    }
    return fnWrite(pDataParam, iSizeParam, iCountParam, pFileParam);
}
