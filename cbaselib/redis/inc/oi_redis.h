#pragma once
#include "hiredis.h"
#include "oi_proto_id.h"
#include "oi_rpc_session_manager.h"
#include "oi_singleton.h"
#include <coroutine>
#include <deque>
#include <memory>
#include <string>
#include <vector>
namespace OI
{
namespace Redis
{

// RESP 字节里不带这个号。号留给以后把几种定制协议放到一条连接上。
enum
{
	REDIS_PROTO_ID = ::OI::Base::PROTO_REDIS
};

enum class RedisStatus
{
	Ok,
	Timeout,
	Disconnected,
	InvalidArgument,
	SendFailed,
	ServerError,
	ProtocolError,
	Busy
};

// reply 共享 hiredis 回执，错误回执也保留，方便业务读取错误文本。
struct RedisResult
{
	std::shared_ptr<redisReply> pReply;
	bool bTimeout;
	RedisStatus iStatus;
	RedisResult()
	    : bTimeout(false), iStatus(RedisStatus::Disconnected)
	{
	}
	int32_t GetRet() const noexcept;
	explicit operator bool() const
	{
		return iStatus == RedisStatus::Ok;
	}
};

class RedisClient;

// 每个调用只允许 await 一次（包括失败和停止）；重复、并发或移走后 await 抛 std::logic_error。
// 调用对象和输出引用须覆盖首次 await 的生命周期，不支持并发移动或销毁。
class RedisCall
{
	friend class RedisClient;
	struct State;
	State* m_pState;

  public:
	RedisCall(std::vector<std::string> vArgumentsParam, RedisResult* pOutputParam);
	RedisCall(RedisCall&& stOtherParam) noexcept;
	RedisCall(const RedisCall&) = delete;
	RedisCall& operator=(const RedisCall&) = delete;
	~RedisCall();
	bool await_ready() const noexcept
	{
		return false;
	}
	template <typename Promise>
	bool await_suspend(std::coroutine_handle<Promise> stHandleParam)
	{
		return Suspend(stHandleParam);
	}
	RedisResult await_resume();

  private:
	bool Suspend(std::coroutine_handle<> stHandleParam);
};

// 跟日志一样走自己的连接，字节是 RESP，不进 proto。
class RedisClient : public ::OI::Rpc::ProtoClientManager
{
	friend class RedisCall;
	DECLARE_SINGLETON(RedisClient)
	RedisClient();
	redisReader* m_pReader;
	sid_t m_iReaderSessionId = INVALID_SID;
	int32_t m_iTimeoutMilliseconds;
	int32_t m_iMaxPending;
	::OI::Thread::Mutex m_stWaitMutex;
	std::deque<RedisCall::State*> m_vWait;
	// _wait 前面这么多条已经判过超时
	size_t m_iSwept;

  public:
	virtual ~RedisClient();
	virtual const std::string GetIDString()
	{
		return "RedisClient";
	}
	virtual bool Init();
	RedisCall Call(std::vector<std::string> vArgumentsParam, RedisResult* pOutputParam);
	// 阻塞到回执或超时。多线程不能在 poll 线程上调。
	// 单线程没有第二个人收包，调用方就在这条线程上把 poll 和协程跑到回执为止。
	RedisResult CallSync(std::vector<std::string> vArgumentsParam);
	size_t PendingCount()
	{
		::OI::Thread::MutexGuard stLock(m_stWaitMutex);
		return m_vWait.size();
	}
	void Feed(sid_t iSessionIdParam, const char* pDataParam, size_t iSizeParam);
	// poll 线程上周期调用，把到期还没回执的调用按超时结掉
	static void SweepTimeout(void*);

  private:
	virtual ::OI::Net::Session* CloneSession();
	virtual void OnClientConnect(::OI::Net::Session* pSessionParam);
	virtual void OnClientDisconnect(const ::OI::Net::Session* pSessionParam);
	static void Submit(void* pStateParam);
	void Enqueue(RedisCall::State* pStParam);
	void FailAll(RedisStatus iStatusParam = RedisStatus::Disconnected);
	void FailSid(sid_t iSessionIdParam);
	void Settle(RedisCall::State* pStParam, std::shared_ptr<redisReply> pReplyParam, RedisStatus iStatusParam);
	static void Release(RedisCall::State* pStParam);
};

inline RedisCall CallRedis(std::vector<std::string> vArgumentsParam, RedisResult& stOutputParam)
{
	return RedisClient::GetInstance().Call(std::move(vArgumentsParam), &stOutputParam);
}

inline RedisCall CallRedis(std::vector<std::string> vArgumentsParam)
{
	return RedisClient::GetInstance().Call(std::move(vArgumentsParam), nullptr);
}

inline RedisResult CallRedisSync(std::vector<std::string> vArgumentsParam)
{
	return RedisClient::GetInstance().CallSync(std::move(vArgumentsParam));
}

}
}
