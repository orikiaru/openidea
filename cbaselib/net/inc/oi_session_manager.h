#pragma once
#include "oi_lock.h"
#include "oi_header.h"
#include "oi_bytes.h"
#include "oi_net_os.h"
#include "oi_session.h"
#include <map>
#include <string>
#include <memory>
#include <vector>
#include <coroutine>
#include <set>
namespace coro
{
void Schedule(std::coroutine_handle<> stHandleParam);
}
namespace OI
{
namespace Net
{

class Session;
enum SESSION_TYPE
{
	SESSION_TYPE_INVALID,
	SESSION_TYPE_TCP,
	//目前只提供TCP
	/*
	SESSION_TYPE_UDP,
	SESSION_TYPE_FIFO,
	SESSION_TYPE_UNIX,
	*/
};
class SessionManager;
class ConnectChannel;

class SessionManager
{
	struct ConnectWait;
	std::vector<std::weak_ptr<ConnectWait>> m_vConnectWaiters;
	void FinishConnectWaiters(bool bResultParam);
	void StartConnect(const std::shared_ptr<ConnectWait>& pWaitParam);
	void BeginConnect(const std::shared_ptr<ConnectWait>& pWaitParam);

  protected:
	typedef std::map<sid_t, Session*> SessionMap;
	SessionMap mSessionMap;
	// 仅 poll 线程访问；旧连接完成/失败不能改变新一代连接状态。
	uint64_t iConnectGeneration = 0;
	bool bConnectPending = false;
	size_t iMaxSendQueueBytes = 64 * 1024 * 1024;
	size_t iMaxReceiveBufferBytes = 64 * 1024 * 1024 + 8;
	size_t iMaxFrameBytes = 64 * 1024 * 1024;
	uint64_t iIncompleteFrameTimeoutMilliseconds = 10000;
	sid_t iCurrentSessionId;
	::OI::Thread::Mutex stLocker;
	std::string sIpAddress;
	std::string sPort;
	SESSION_TYPE iSessionType; //会话管理器管理的会话类型,TCP,UDP,UNIX,FIFO
	std::string sServiceName;
	std::string sEndpoint;
	bool bServiceDown;
	bool bServiceSeen;

  public:
	// 等待连接的 awaiter；只挂起调用协程，完成后进入框架调度队列。
	class ConnectAwaiter
	{
		std::shared_ptr<ConnectWait> m_pWait;
		bool Suspend(std::coroutine_handle<> stHandleParam, void (*fnScheduleParam)(std::coroutine_handle<>));

	  public:
		explicit ConnectAwaiter(SessionManager* pManagerParam);
		ConnectAwaiter(const ConnectAwaiter&) = delete;
		ConnectAwaiter(ConnectAwaiter&&) = default;
		bool await_ready() const noexcept
		{
			return false;
		}
		bool await_suspend(std::coroutine_handle<> stHandleParam)
		{
			return Suspend(stHandleParam, coro::Schedule);
		}
		bool await_resume() const noexcept;
	};
	SessionManager()
	    : iCurrentSessionId(0), iSessionType(SESSION_TYPE_TCP), bServiceDown(false), bServiceSeen(false)
	{
	}
	virtual ~SessionManager();
	// 管理器常驻；入站任务仅持有连接的业务租约。
	std::shared_ptr<void> HoldSessionTask(sid_t iSessionIdParam);
	size_t MaxSendQueueBytes() const
	{
		return iMaxSendQueueBytes;
	}
	size_t MaxReceiveBufferBytes() const
	{
		return iMaxReceiveBufferBytes;
	}
	void DefaultInit(const std::string& sSipParam, const std::string& sPortParam, SESSION_TYPE iSessionTypeParam = SESSION_TYPE_TCP)
	{
		sIpAddress = sSipParam;
		sPort = sPortParam;
		iSessionType = iSessionTypeParam;
	}
	size_t MaxFrameBytes() const
	{
		return iMaxFrameBytes;
	}
	uint64_t IncompleteFrameTimeoutMilliseconds() const
	{
		return iIncompleteFrameTimeoutMilliseconds;
	}
	const std::string& GetStrIP()
	{
		return sIpAddress;
	}
	const std::string& GetStrPort()
	{
		return sPort;
	}
	SESSION_TYPE GetSessionType()
	{
		return iSessionType;
	}
	// 调用即启动；忽略结果为异步启动，co_await 等待连接成功/失败。
	ConnectAwaiter StartClient()
	{
		return ConnectAwaiter(this);
	}
	bool StartServer(bool bAsyncParam = true); //目前不支持同步操作
	// 定期拉到的服务列表。online 为假表示这个名字已经从列表里消失。
	void ApplyService(const std::string& sIpParam, const std::string& sPortParam, bool bOnlineParam);
	void CloseAllSessions();
	void CloseEndpoint(const std::string& sEndpointParam);
	bool HasEndpoint(const std::string& sEndpointParam);

	bool SendBytes(sid_t iSessionIdParam, const ::OI::Base::Bytes& stDataParam);
	int32_t SendBytesResult(sid_t iSessionIdParam, const ::OI::Base::Bytes& stDataParam);

	static SESSION_TYPE GetSessionTypeByString(const std::string& sTypeParam);

  public:
	//主动关闭会话
	void CloseSession(sid_t iSessionIdParam);

  protected:
	friend class NetSession;
	friend class AcceptChannel;
	friend class ConnectChannel;
	friend class NetHelper;
	//AcceptSession调用,必然由IO线程触发
	void AddSession(net_socket_t iNewDescriptorParam);
	void AddSession(Session* pSessionParam);
	void AddSession(Session* pSessionParam, const std::string& sEndpointParam, size_t iSendBytesParam, size_t iReceiveBytesParam);
	//ConnectSession调用,必然有IO线程触发
	void AbortSession(uint64_t iGenerationParam);
	void CompleteConnect(net_socket_t iDescriptorParam, uint64_t iGenerationParam);
	//不用怕重入,必然由IO线程触发
	void DelSession(const Session* pSessionParam);

	/*---------------以下是需要重写的内容--------------*/
  public:
	virtual bool Init();
	virtual const std::string GetIDString();

  protected:
	virtual Session* CloneSession() = 0; //{ 	return new TCPSession(fd, this);	}	//自定制回话类型
	virtual void OnAddSession(Session* pSessionParam) = 0;
	virtual void OnDelSession(const Session* pSessionParam) = 0;
	virtual void OnSessionClosed(sid_t)
	{
	}
	virtual void OnConnectFailed()
	{
	}
	virtual void OnServiceDown()
	{
	}

  public:
	// 会话还没建立过时把调用先挂上。返回 false 表示调用方照常发送。
	virtual int32_t UnavailableCode()
	{
		return OI_ERR_FAIL;
	}
	virtual bool TryPendCall(uint64_t, const ::OI::Base::Bytes&)
	{
		return false;
	}
	virtual void DropPendingCall(uint64_t)
	{
	}

  protected:
};
}
}
