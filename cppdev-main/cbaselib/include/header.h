#ifndef _j_header_h__
#define _j_header_h__
#include <stdint.h>

#define FUNCTION_RUN_BEFORE_MAIN __attribute__((constructor))
#define CONSTRUCTOR_FUNC(X) \
	void __constructor_func_##X() FUNCTION_RUN_BEFORE_MAIN;\
        void __constructor_func_##X()

#define SAFE_FREE(x) if(x)	{ free(x); x = NULL; }
#define SAFE_DELETE(x) if(x) { delete x; x = NULL; }

#define INVALID_SID -1
typedef int sid_t;
typedef int64_t rpc_id_t;
typedef int proto_type_t;
static const int64_t RPC_REQ_MASK = 0x8000000000000000L;	//最高位用来标记请求

//此处协议枚举也需要生成~

//用来与linux错误码和其他模块错误区分
#define NET_PARSER_ERR_BEGIN 10000

enum PARSER_ERR_ENUM
{
	PARSER_ERR_SUCCESS = 0,				//解析成功,没有错误默认0
	PARSER_ERR_UNFINISH = NET_PARSER_ERR_BEGIN,	//没有接收完全
	PARSER_ERR_PROTOCOL_TYPE,			//无效的协议类型
	PARSER_ERR_PROTOCOL_SIZE,			//无效的协议大小
	PARSER_ERR_OTHER,	
};

#define SESSION_MANAGER_ERR_BEGIN 20000
enum
{
	SESSION_MANAGER_NORMAL_CLOSE = SESSION_MANAGER_ERR_BEGIN,
};

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
