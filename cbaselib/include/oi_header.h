#ifndef _j_header_h__
#define _j_header_h__
#include <stdint.h>

#ifdef _MSC_VER
#ifndef __attribute__
#define __attribute__(x)
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGQUIT
#define SIGQUIT 3
#endif
#ifndef SIGALRM
#define SIGALRM 14
#endif
#ifndef EINPROGRESS
#define EINPROGRESS 112
#endif
#endif

#define FUNCTION_RUN_BEFORE_MAIN __attribute__((constructor))
#define CONSTRUCTOR_FUNC(X) \
	void __constructor_func_##X() FUNCTION_RUN_BEFORE_MAIN;\
        void __constructor_func_##X()

#define SAFE_FREE(x) if(x)	{ free(x); x = NULL; }
#define SAFE_DELETE(x) if(x) { delete x; x = NULL; }

#define INVALID_SID -1
typedef int32_t sid_t;
typedef int64_t rpc_id_t;
typedef int32_t proto_type_t;
static const int64_t RPC_REQ_MASK = 0x8000000000000000L;	//最高位用来标记请求

//此处协议枚举也需要生成~

#include "oi_errno.h"

enum LOG_LEVEL
{
	LOG_TRACE,
	LOG_DEBUG,
	LOG_INFO,
	LOG_WARNING,
	LOG_ERROR,
	LOG_LEVEL_COUNT,
};

#endif
