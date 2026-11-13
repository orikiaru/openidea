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
namespace OI
{
namespace Coro
{
void Schedule(std::coroutine_handle<> stHandle);
}
}
namespace OI
{
namespace Base
{
struct ServiceInstance;
}
namespace Net
{

class Session;
enum SESSION_TYPE
{
	SESSION_TYPE_INVALID,
	SESSION_TYPE_TCP,
	// 一个绑好的口，按对端地址分会话。只有 StartServer，没有 StartClient，主动发用 OpenPeer。
	SESSION_TYPE_UDP,
	//还没做
	/*
	SESSION_TYPE_FIFO,
	SESSION_TYPE_UNIX,
	*/
};
class SessionManager;
class ConnectChannel;
class UDP_Channel;
class NetAddr;

class SessionManager
{
	struct ConnectWait;
	// 正在等这一代连接结果的协程。完成或失败时一次唤醒。
	std::vector<std::weak_ptr<ConnectWait>> m_vConnectWaiters;
	// 把这一代等待者全部唤醒。iResult 是本次连接的错误码。
	void FinishConnectWaiters(int32_t iResult);
	// 在 poll 线程上登记等待者并发起连接。已经连上就直接成功。
	void StartConnect(const std::shared_ptr<ConnectWait>& pWait);
	friend struct ConnectResolver;
	// 切到 poll 线程再调用 StartConnect。停止中直接失败。
	void BeginConnect(const std::shared_ptr<ConnectWait>& pWait);

  protected:
	typedef std::map<sid_t, Session*> SessionMap;
	// 会话号到会话。
	SessionMap mSessionMap;
	// 仅 poll 线程访问；旧连接完成/失败不能改变新一代连接状态。
	uint64_t iConnectGeneration = 0;
	// 这一代连接还没完成。
	bool bConnectPending = false;
	// 新建会话的发送队列上限。
	size_t iMaxSendQueueBytes = 64 * 1024 * 1024;
	// 新建会话的接收缓冲上限。
	size_t iMaxReceiveBufferBytes = 64 * 1024 * 1024 + 8;
	// 单帧上限。再大的帧直接关掉会话。
	size_t iMaxFrameBytes = 64 * 1024 * 1024;
	// 半帧超过这么久就关掉会话。
	uint64_t iIncompleteFrameTimeoutMilliseconds = 10000;
	// UDP 会话这么久没收没发就关掉。
	uint64_t iIdleTimeoutMilliseconds = 60000;
	// UDP 的本端口。StartServer 之后才有。
	UDP_Channel* pUdpChannel = NULL;
	// 下一个要分配的会话号。
	sid_t iCurrentSessionId;
	// 保护会话表和连接状态。
	::OI::Thread::Mutex stLocker;
	// 监听或要连接的地址。客户端可以是域名，连上之前会换成解析出来的数字地址。
	std::string sIpAddress;
	std::string sPort;
	SESSION_TYPE iSessionType; // 管理的会话类型。TCP 或 UDP。
	// 在注册表里的服务名。空表示地址来自配置，不走发现。
	std::string sServiceName;
	// 当前端点文本。IPv4 是 ip:port，IPv6 是 [ip]:port。
	std::string sEndpoint;
	// 服务名已经从列表消失，不再连接。
	bool bServiceDown;
	// 曾经在列表里见过这个名字。
	bool bServiceSeen;

  public:
	// 等待连接的 awaiter；只挂起调用协程，完成后进入框架调度队列。
	class ConnectAwaiter
	{
		// 这一次等待的共享状态。连接完成时用它唤醒协程。
		std::shared_ptr<ConnectWait> m_pWait;
		bool Suspend(std::coroutine_handle<> stHandle, void (*fnSchedule)(std::coroutine_handle<>));

	  public:
		explicit ConnectAwaiter(SessionManager* pManager);
		ConnectAwaiter(const ConnectAwaiter&) = delete;
		ConnectAwaiter(ConnectAwaiter&&) = default;
		bool await_ready() const noexcept
		{
			return false;
		}
		bool await_suspend(std::coroutine_handle<> stHandle)
		{
			return Suspend(stHandle, ::OI::Coro::Schedule);
		}
		int32_t await_resume() const noexcept;
	};
	SessionManager()
	    : iCurrentSessionId(0), iSessionType(SESSION_TYPE_TCP), bServiceDown(false), bServiceSeen(false)
	{
	}
	virtual ~SessionManager();
	// 管理器常驻；入站任务仅持有连接的业务租约。
	std::shared_ptr<void> HoldSessionTask(sid_t iSessionId);
	size_t MaxSendQueueBytes() const
	{
		return iMaxSendQueueBytes;
	}
	size_t MaxReceiveBufferBytes() const
	{
		return iMaxReceiveBufferBytes;
	}
	void DefaultInit(const std::string& sSip, const std::string& sPortLocal, SESSION_TYPE iSessionTypeLocal = SESSION_TYPE_TCP)
	{
		sIpAddress = sSip;
		sPort = sPortLocal;
		iSessionType = iSessionTypeLocal;
	}
	size_t MaxFrameBytes() const
	{
		return iMaxFrameBytes;
	}
	uint64_t IncompleteFrameTimeoutMilliseconds() const
	{
		return iIncompleteFrameTimeoutMilliseconds;
	}
	uint64_t IdleTimeoutMilliseconds() const
	{
		return iIdleTimeoutMilliseconds;
	}
	// UDP：实际绑上的本端端口。还没 StartServer 返回 0。
	uint16_t LocalPort() const;
	// UDP：找到或新建到这个字面地址的会话，成功写入会话号。不解析域名，失败返回具体错误码。
	int32_t OpenPeer(const std::string& sIp, const std::string& sPortLocal, sid_t& iSessionId);
	// UDP：会话的对端地址。会话不在返回 OI_ERR_NOT_FOUND。
	int32_t GetPeerAddr(sid_t iSessionId, NetAddr& stAddr);
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
	int32_t StartServer(bool bAsync = true); //目前不支持同步操作
	// 定期拉到的服务列表。online 为假表示这个名字已经从列表里消失。
	void ApplyService(const std::string& sIp, const std::string& sPortLocal, bool bOnline);
	// 关注整份名单的管理器收这个名字下的全部实例，名单有变化时才调用。默认什么都不做。
	virtual void ApplyServiceList(const std::vector<::OI::Base::ServiceInstance>&)
	{
	}
	// 关掉当前所有会话。
	int32_t CloseAllSessions();
	// 关掉这个地址上的会话。
	int32_t CloseEndpoint(const std::string& sEndpoint);
	// 是否已经有连到这个地址的会话。
	bool HasEndpoint(const std::string& sEndpoint);

	// 按会话号发送。成功返回 OI_OK。
	int32_t SendBytes(sid_t iSessionId, const ::OI::Base::Bytes& stData);
	// 按会话号发送。会话不在或队列满时返回错误码。
	int32_t SendBytesResult(sid_t iSessionId, const ::OI::Base::Bytes& stData);

	// 把配置里的类型字符串换成枚举。认不出返回 SESSION_TYPE_INVALID。
	static SESSION_TYPE GetSessionTypeByString(const std::string& sType);

  public:
	//主动关闭会话
	// 按会话号主动关闭。
	int32_t CloseSession(sid_t iSessionId);

  protected:
	friend class NetSession;
	friend class AcceptChannel;
	friend class ConnectChannel;
	friend class UDP_Channel;
	friend class NetHelper;
	//AcceptSession调用,必然由IO线程触发
	// poll 线程上用新套接字建一条会话并纳入表。
	void AddSession(net_socket_t iNewDescriptor);
	// poll 线程上纳入一条已经建好的会话。
	void AddSession(Session* pSession);
	// 同上，并带上对端地址和这条会话自己的收发上限。
	void AddSession(Session* pSession, const std::string& sEndpoint, size_t iSendBytes, size_t iReceiveBytes);
	// 这一代连接失败。代次对不上就忽略，避免旧连接改掉新连接。
	void AbortSession(uint64_t iGeneration, int32_t iCode = OI_ERR_CONNECT);
	// 放弃还没完成的这一代。晚到的解析或连接结果作废。
	void CancelConnect();
	// 这一代连接成功，装上新套接字。代次对不上就关掉新描述符。
	void CompleteConnect(net_socket_t iDescriptor, uint64_t iGeneration);
	//不用怕重入,必然由IO线程触发
	void DelSession(const Session* pSession);

	/*---------------以下是需要重写的内容--------------*/
  public:
	// 子类读自己的配置。默认成功。
	virtual int32_t Init();
	// 日志里用的管理器名字。
	virtual const std::string GetIDString();

  protected:
	// 配了 service 时在 Init 里调用。默认按名字查一台，查到就改写 ip/port。
	virtual void BindService(const std::string& sService, std::string& sIpLocal, std::string& sPortLocal);
	// Init 末尾调用。默认跟着名单里选中的那一台切换地址。
	virtual void WatchService(const std::string& sService);
	// 按本模块的会话类型新建一条。
	virtual Session* CloneSession() = 0;
	// 会话进表时通知子类。
	virtual void OnAddSession(Session* pSession) = 0;
	// 会话出表时通知子类。
	virtual void OnDelSession(const Session* pSession) = 0;
	// 会话号关闭后的通知。默认什么都不做。
	virtual void OnSessionClosed(sid_t)
	{
	}
	// 连接失败。默认什么都不做。
	virtual void OnConnectFailed()
	{
	}
	// 服务名从列表消失。默认什么都不做。
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
