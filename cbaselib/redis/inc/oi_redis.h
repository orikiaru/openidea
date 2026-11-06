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
	// hiredis 回执。错误回执也留着，方便读错误文本。
	std::shared_ptr<redisReply> pReply;
	// 这次是超时，不是对端关掉。
	bool bTimeout;
	// 调用结果。只有 Ok 表示成功。
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
	// 命令参数、输出位置和完成门。移动后原对象把这里置空。
	State* m_pState;

  public:
	RedisCall(std::vector<std::string> vArguments, RedisResult* pOutput);
	RedisCall(RedisCall&& stOther) noexcept;
	RedisCall(const RedisCall&) = delete;
	RedisCall& operator=(const RedisCall&) = delete;
	~RedisCall();
	bool await_ready() const noexcept
	{
		return false;
	}
	template <typename Promise>
	bool await_suspend(std::coroutine_handle<Promise> stHandle)
	{
		return Suspend(stHandle);
	}
	RedisResult await_resume();

  private:
	bool Suspend(std::coroutine_handle<> stHandle);
};

// 跟日志一样走自己的连接，字节是 RESP，不进 proto。
class RedisClient : public ::OI::Rpc::ProtoClientManager
{
	friend class RedisCall;
	DECLARE_SINGLETON(RedisClient)
	RedisClient();
	// 当前会话上的 RESP 解析器。换会话就换一个。
	redisReader* m_pReader;
	// m_pReader 对应的会话。对不上的字节丢掉。
	sid_t m_iReaderSessionId = INVALID_SID;
	// 一条调用最多等多久，毫秒。
	int32_t m_iTimeoutMilliseconds;
	// 在途调用上限。满了新调用返回忙。
	int32_t m_iMaxPending;
	// 保护等待队列。
	::OI::Thread::Mutex m_stWaitMutex;
	// 已发出、按顺序等回执的调用。
	std::deque<RedisCall::State*> m_vWait;
	// _wait 前面这么多条已经判过超时
	size_t m_iSwept;

  public:
	virtual ~RedisClient();
	virtual const std::string GetIDString()
	{
		return "RedisClient";
	}
	// 读 Redis 地址、超时和在途上限。
	virtual bool Init();
	// 组一条 RESP 调用。pOutput 可空，空则结果只留在 co_await 的返回值里。
	RedisCall Call(std::vector<std::string> vArguments, RedisResult* pOutput);
	// 阻塞到回执或超时。多线程不能在 poll 线程上调。
	// 单线程没有第二个人收包，调用方就在这条线程上把 poll 和协程跑到回执为止。
	RedisResult CallSync(std::vector<std::string> vArguments);
	// 还在等回执的调用数。
	size_t PendingCount()
	{
		::OI::Thread::MutexGuard stLock(m_stWaitMutex);
		return m_vWait.size();
	}
	// 把会话上新到的字节喂给解析器，拆出的回执按顺序结掉等待。
	void Feed(sid_t iSessionId, const char* pData, size_t iSize);
	// poll 线程上周期调用，把到期还没回执的调用按超时结掉
	static void SweepTimeout(void*);

  private:
	// 新建一条 Redis 会话。
	virtual ::OI::Net::Session* CloneSession();
	// 换上新的解析器，并把还在等的调用按断开结掉。
	virtual void OnClientConnect(::OI::Net::Session* pSession);
	// 这条会话断开时，结掉记在它上面的调用。
	virtual void OnClientDisconnect(const ::OI::Net::Session* pSession);
	// 连接就绪后把调用发出去。
	static void Submit(void* pState);
	// 放进按顺序等回执的队列。满了就按忙结掉。
	void Enqueue(RedisCall::State* pSt);
	// 把还在等的调用全部按给定状态结掉。
	void FailAll(RedisStatus iStatus = RedisStatus::Disconnected);
	// 只结掉这个会话上的调用。
	void FailSid(sid_t iSessionId);
	// 写回执并按完成门唤醒等待的协程。
	void Settle(RedisCall::State* pSt, std::shared_ptr<redisReply> pReply, RedisStatus iStatus);
	// 调用状态没有引用后释放。
	static void Release(RedisCall::State* pSt);
};

// 协程里发一条命令，结果同时写进 stOutput。
inline RedisCall CallRedis(std::vector<std::string> vArguments, RedisResult& stOutput)
{
	return RedisClient::GetInstance().Call(std::move(vArguments), &stOutput);
}

// 协程里发一条命令。结果只在 co_await 的返回值里。
inline RedisCall CallRedis(std::vector<std::string> vArguments)
{
	return RedisClient::GetInstance().Call(std::move(vArguments), nullptr);
}

// 阻塞到回执或超时。poll 回调里调用会抛 logic_error。
inline RedisResult CallRedisSync(std::vector<std::string> vArguments)
{
	return RedisClient::GetInstance().CallSync(std::move(vArguments));
}

}
}
