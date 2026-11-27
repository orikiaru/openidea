#pragma once
#include <string>
#include <atomic>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "oi_session_manager.h"
#include "oi_lock.h"
#include "oi_rpc_coro.h"
#include "oi_rpc_session.h"
#include "oi_rpc_call.h"
#include "oi_rpc_peer.h"
#include "oi_registry.h"
#include "oi_singleton.h"
#include "oi_log.h"
#include "oi_duration.h"
#include "oi_poll.h"
namespace OI
{
namespace Rpc
{

enum CLIENT_CONNECT_MODE
{
	CLIENT_CONNECT_DIRECT = 0,
	CLIENT_CONNECT_ON_SEND = 1,
};

class ProtoSessionManager : public ::OI::Net::SessionManager
{
	// 只有本管理器会话上进来的请求查这张表。登记后不改不删，查到的指针出锁后仍可用。
	::OI::Thread::Mutex m_stHandlerLocker;
	std::map<proto_type_t, RpcHandleFn> m_mHandlers;
	void OnSessionClosed(sid_t iSessionId) override final
	{
		RpcCallManager::GetInstance().FailSession(this, iSessionId);
	}

  public:
	// 同一管理器上一个协议号只能登记一次，重复抛 std::logic_error，原处理函数不变。
	void BindRpcHandler(proto_type_t iId, RpcHandleFn fnHandle);
	// 没登记返回空。
	const RpcHandleFn* FindRpcHandler(proto_type_t iId);
	// 处理函数形参是 (req, res) 或 (const RpcContext&, req, res)，类型从形参推出；
	// 也可传本管理器类的成员函数，如 RegisterRpcHandler(&IpcServer::HandlePeerProbe)。带 auto 形参的写 RegisterRpcHandler<Req>。
	// 成员函数不在注册时找对象：登记时确认本管理器就是 C，处理时直接调本管理器的成员。
	template <typename F>
	void RegisterRpcHandler(F fnFunction)
	{
		static_assert(kRpcHandlerDeducible<F>, "RPC handler must accept (req, res) or (const RpcContext&, req, res); generic lambdas need RegisterRpcHandler<Req>");
		using Args = typename RpcHandlerArgs<F>::Args;
		using Req = typename RpcHandlerTypes<Args>::Request;
		using Res = typename RpcHandlerTypes<Args>::Response;
		static_assert(std::is_same_v<Res, typename RpcProtoId<Req>::Response>, "RPC handler response type does not match the request");
		if constexpr (std::is_member_function_pointer_v<F>)
		{
			using C = typename RpcCallableArgs<F>::Class;
			using R = typename RpcCallableArgs<F>::Return;
			static_assert(std::is_base_of_v<ProtoSessionManager, C>, "RPC member handler must belong to a session manager class");
			C* pSelf = dynamic_cast<C*>(this);
			if (pSelf == nullptr)
			{
				LOG_ERROR("rpc member handler class mismatch manager=%s proto=%u", GetIDString().c_str(), (unsigned)RpcProtoId<Req>::iValue);
				fprintf(stderr, "rpc member handler class mismatch manager=%s proto=%u\n", GetIDString().c_str(), (unsigned)RpcProtoId<Req>::iValue);
				fflush(stderr);
				throw std::logic_error("rpc member handler class mismatch manager=" + GetIDString() + " proto=" + std::to_string((unsigned)RpcProtoId<Req>::iValue));
			}
			RegisterRpcHandler<Req, Res>([pSelf, fnFunction](const RpcContext& stContext, Req& stRequest, Res& stResponse) -> R
			{
				if constexpr (std::tuple_size_v<Args> == 3)
				{
					return (pSelf->*fnFunction)(stContext, stRequest, stResponse);
				}
				else
				{
					return (pSelf->*fnFunction)(stRequest, stResponse);
				}
			});
		}
		else
		{
			RegisterRpcHandler<Req, Res>(std::move(fnFunction));
		}
	}
	template <typename Req, typename F>
	void RegisterRpcHandler(F fnFunction)
	{
		RegisterRpcHandler<Req, typename RpcProtoId<Req>::Response>(std::move(fnFunction));
	}
	template <typename Req, typename Res, typename F>
	void RegisterRpcHandler(F fnFunction)
	{
		BindRpcHandler(RpcProtoId<Req>::iValue, RpcHandlerBuilder::Make<Req, Res>(std::move(fnFunction), RpcProtoId<Res>::iValue));
	}
	// 单向消息，形参是 (msg) 或 (const RpcContext&, msg)，同样可传成员函数。
	template <typename F>
	void RegisterNotifyHandler(F fnFunction)
	{
		static_assert(kRpcNotifyDeducible<F>, "notify handler must accept (msg) or (const RpcContext&, msg); generic lambdas need RegisterNotifyHandler<Msg>");
		using Args = typename RpcHandlerArgs<F>::Args;
		using Msg = typename RpcNotifyTypes<Args>::Message;
		if constexpr (std::is_member_function_pointer_v<F>)
		{
			using C = typename RpcCallableArgs<F>::Class;
			using R = typename RpcCallableArgs<F>::Return;
			static_assert(std::is_base_of_v<ProtoSessionManager, C>, "notify member handler must belong to a session manager class");
			C* pSelf = dynamic_cast<C*>(this);
			if (pSelf == nullptr)
			{
				LOG_ERROR("notify member handler class mismatch manager=%s proto=%u", GetIDString().c_str(), (unsigned)RpcProtoId<Msg>::iValue);
				fprintf(stderr, "notify member handler class mismatch manager=%s proto=%u\n", GetIDString().c_str(), (unsigned)RpcProtoId<Msg>::iValue);
				fflush(stderr);
				throw std::logic_error("notify member handler class mismatch manager=" + GetIDString() + " proto=" + std::to_string((unsigned)RpcProtoId<Msg>::iValue));
			}
			RegisterNotifyHandler<Msg>([pSelf, fnFunction](const RpcContext& stContext, Msg& stMessage) -> R
			{
				if constexpr (std::tuple_size_v<Args> == 2)
				{
					return (pSelf->*fnFunction)(stContext, stMessage);
				}
				else
				{
					return (pSelf->*fnFunction)(stMessage);
				}
			});
		}
		else
		{
			RegisterNotifyHandler<Msg>(std::move(fnFunction));
		}
	}
	// 调用号 0 不回包；RPC 包装通知复用默认响应与错误处理。
	template <typename Msg, typename F>
	void RegisterNotifyHandler(F fnFunction)
	{
		using Response = typename RpcProtoId<Msg>::Response;
		auto fnAdapter = [fnFunction = std::move(fnFunction)](const RpcContext& stContext, Msg& stMessage, Response&) mutable
		{
			if constexpr (std::is_invocable_v<F&, Msg&>)
			{
				return fnFunction(stMessage);
			}
			else
			{
				return fnFunction(stContext, stMessage);
			}
		};
		BindRpcHandler(RpcProtoId<Msg>::iValue, RpcHandlerBuilder::Make<Msg, Response>(std::move(fnAdapter), RpcProtoId<Msg>::iValue));
	}
	ProtoSessionManager()
	{
	}
	int32_t Init() override;
	const std::string GetIDString() override
	{
		return "ProtoSessionManager";
	}
	// 向这条会话发一条单向消息。sid 从 ConnectedSid、OpenPeer 或 RpcContext 来。
	template <typename Msg>
	int32_t Notify(sid_t iSessionId, const Msg& stMessage)
	{
		return RpcNotify(*this, iSessionId, stMessage);
	}
	::OI::Net::Session* CloneSession() override
	{
		return new ProtoSession(this);
	} //自定制回话类型
	void OnAddSession(::OI::Net::Session* pSession) override;
	void OnDelSession(const ::OI::Net::Session* pSession) override;
	// 固定地址连接建好后纳入本管理器的会话表，只在 poll 线程调用。
	sid_t AdoptPeer(net_socket_t iSocketLocal, const std::shared_ptr<PeerConnection>& pConnection);
	// 固定地址连接连上、连接前暂存的请求补发之后回调，在 poll 线程。默认什么都不做。
	virtual void OnPeerConnect(int32_t, sid_t)
	{
	}
};

// client 只连一个 server，自己维护长连接状态：连上握手、感知断开、发原始帧、连上前暂存。
// 按服务名或分区的无状态调用走 RouteManager；同一服务名下每台实例各连一条，用 ProtoMultiClientManager。
class ProtoClientManager : public ProtoSessionManager
{
	RpcSender m_stSender;
	sid_t m_iSessionId;
	bool m_bConnected;
	CLIENT_CONNECT_MODE m_iConnectMode;
	struct HeldCall
	{
		uint64_t iCallId;
		::OI::Base::Bytes stData;
	};
	std::deque<::OI::Base::Bytes> m_vPending;
	std::list<HeldCall> m_vHeldCalls;
	std::unordered_map<uint64_t, std::list<HeldCall>::iterator> m_mHeldIndex;
	size_t m_iHeldBytes = 0;
	size_t m_iHeldMaxCount = 65536;
	size_t m_iHeldMaxBytes = 64 * 1024 * 1024;
	size_t m_iPendingBytes;
	bool m_bEverConnected;
	::OI::Thread::SpinLock m_stClientLocker;
	bool HoldCallLocked(uint64_t iCallId, const ::OI::Base::Bytes& stData);

  protected:
	virtual bool AutoReconnect() const
	{
		return true;
	}

  public:
	ProtoClientManager();
	RpcSender& Sender()
	{
		return m_stSender;
	}
	void SetConnectMode(CLIENT_CONNECT_MODE iMode)
	{
		m_iConnectMode = iMode;
	}
	CLIENT_CONNECT_MODE GetConnectMode() const
	{
		return m_iConnectMode;
	}
	virtual void OnClientConnect(::OI::Net::Session* pSession) = 0;
	virtual void OnClientDisconnect(const ::OI::Net::Session* pSession) = 0;

	sid_t ConnectedSid();
	bool IsConnect();
	void OnAddSession(::OI::Net::Session* pSession) override;
	void OnConnectFailed() override;
	void OnServiceDown() override
	{
		FailHeldCalls();
	}
	int32_t UnavailableCode() override
	{
		return IsConnect() ? OI_ERR_FAIL : OI_ERR_CONNECT;
	}
	void SetPendingCallLimits(size_t iCount, size_t iBytes);
	std::pair<size_t, size_t> PendingCallLimits();
	bool TryPendCall(uint64_t iCallId, const ::OI::Base::Bytes& stData) override;
	// 提交时在客户端锁内决定排队或绑定当前 sid，随后发送，避免构造等待对象时截取旧 sid。
	virtual void SubmitCall(uint64_t iCallId, const ::OI::Base::Bytes& stData);
	void DropPendingCall(uint64_t iCallId) override;
	void FlushHeldCalls(sid_t iSessionId);
	void FailHeldCalls();
	void OnDelSession(const ::OI::Net::Session* pSession) override;
	// 在本管理器这条连接上调用，沿用首次建连排队和固定配置。自动配对响应类型，返回 TaskResult<int32_t>；输出须活到任务结束。
	template <typename Req>
	[[nodiscard]] auto Call(Req stRequest, typename RpcProtoId<Req>::Response& stResponse, ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180))
	{
		return RpcCall(m_stSender, std::move(stRequest), stResponse, stTimeout);
	}
	using ProtoSessionManager::Notify;
	// 发到本管理器这条连接，没连上先暂存、连上补发，和 Call 一样。返回 OI_OK 只表示任务已启动，发送或回执失败由任务错误处理器报告。
	template <typename Msg>
	int32_t Notify(const Msg& stMessage)
	{
		// 管理器常驻，不拥有发送目标。
		return PeerCall::StartNotify(std::shared_ptr<RpcSender>(std::shared_ptr<RpcSender>(), &m_stSender), stMessage);
	}
	int32_t SendRaw(const ::OI::Base::Bytes& stData);
	int32_t SendRawResult(const ::OI::Base::Bytes& stData);
	// 没连上先存着，连上时按顺序补发。超过 max_pending_bytes 返回 OI_ERR_BUSY，不入队
	int32_t SendRawOrPend(const ::OI::Base::Bytes& stData, size_t iMaxPendingBytes);
	int32_t SendRawOrPendResult(const ::OI::Base::Bytes& stData, size_t iMaxPendingBytes);
	int32_t Reconnect(int iDelayMilliseconds = 2000);
};

// 常驻应用管理器用成员发送目标适配，不需要继承发送接口。
template <typename Req>
auto MakeRpcCallAwaiter(ProtoClientManager& stManager, Req& stRequest, typename RpcProtoId<Req>::Response& stResponse,
    ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180))
{
	return MakeRpcCallAwaiter(stManager.Sender(), stRequest, stResponse, stTimeout);
}

template <typename Req>
[[nodiscard]] auto RpcCall(ProtoClientManager& stManager, Req stRequest, typename RpcProtoId<Req>::Response& stResponse,
    ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180))
{
	return RpcCall(stManager.Sender(), std::move(stRequest), stResponse, stTimeout);
}

template <typename Req>
[[nodiscard]] auto RpcCall(ProtoClientManager& stManager, Req stRequest,
    ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180))
{
	return RpcCall(stManager.Sender(), std::move(stRequest), stTimeout);
}

// 同一服务名下每台实例各一条常驻连接，断开或连不上自动重连。
// 配了 <段名>.service：跟着注册表的整份名单，实例号取登记的 [Service] id；注册里查不到时先连配置地址。
// 配置地址：<段名>.instances = ["实例号 ip 端口 [分区]", ...]，配了它就不要求 ip/port；
// 没配 instances 时只有一条连到 <段名>.ip/port 的连接，实例号是 0。
class ProtoMultiClientManager : public ProtoSessionManager
{
	::OI::Thread::Mutex m_stPeerLocker;
	std::vector<::OI::Base::ServiceInstance> m_vSeed;
	bool m_bListed = false;
	ServiceGroup m_stGroup;
	// 读配置地址：instances 或 ip/port。instances 格式不对、实例号重复或为空返回 OI_ERR_INVALID_ARGUMENT。
	int32_t LoadInstances(std::vector<::OI::Base::ServiceInstance>& vList);

  protected:
	void BindService(const std::string& sService, std::string& sIpLocal, std::string& sPortLocal) override;
	void WatchService(const std::string& sService) override;
	bool AddressOptional() override;
	void OnDelSession(const ::OI::Net::Session* pSession) override;
	// 连不上之后隔多久再连。
	virtual int RetryMilliseconds() const
	{
		return 2000;
	}
	// 某台断开，在 poll 线程。这台的在途调用已经回 OI_ERR_CONNECT。
	virtual void OnPeerDisconnect(int32_t, sid_t)
	{
	}

  public:
	ProtoMultiClientManager();
	int32_t Init() override;
	// 对当前名单里的每台发起建连。之后新登记的实例自动连。
	int32_t Start();
	// 单地址建连不适用于这里。
	OI::Coro::TaskResult<int32_t> StartClient() = delete;
	// 写死地址时换一台，Init 之后调用：旧连接退役，在途调用回 OI_ERR_CONNECT；实例号仍是 0，Start 过就马上连新地址。
	// 配了 service 或地址为空返回 OI_ERR_INVALID_ARGUMENT。地址没变什么都不做。
	int32_t UseAddress(const std::string& sIp, const std::string& sPortLocal);
	// 名单里至少有一台连着。
	bool IsConnect();
	// 当前名单，按实例号升序。
	std::vector<int32_t> InstanceIds();
	// 没这台或没连上返回 INVALID_SID。
	sid_t ConnectedSid(int32_t iInstanceId);
	// 这台还没发出、等连接的暂存请求。没这台返回全 0。
	PeerHeldInfo HeldInfo(int32_t iInstanceId);
	// 名单里没有这台返回空。
	std::shared_ptr<RpcSender> PeerSender(int32_t iInstanceId);
	// 在已连上的实例里轮询；一台都没连上时在名单里轮询，调用先暂存。名单为空返回空。
	std::shared_ptr<RpcSender> AnySender();
	// 只在已连上的里轮询，返回这台当前的会话号。一台都没连上返回 INVALID_SID。
	sid_t ConnectedAny();
	// 按实例号升序取当前名单的发送目标，vIds 与 stRoute.vClients 一一对应。
	void AllSenders(std::vector<int32_t>& vIds, ServiceRoute& stRoute);
	// 按给定实例号取发送目标，顺序一致；名单里没有的那一格是空指针。
	void ManySenders(const std::vector<int32_t>& vInstanceIds, ServiceRoute& stRoute);
	// 注册表下发这个名字的整份名单。按实例号比对：多出的建连，消失的退役，地址变了的先退役再新建。
	void ApplyServiceList(const std::vector<::OI::Base::ServiceInstance>& vList) override;

	// 向指定实例启动异步通知任务；名单里没有返回 OI_ERR_CONNECT，未连接时暂存并重连。
	// 返回 OI_OK 只表示任务已启动，发送或回执失败由任务错误处理器报告。
	template <typename Msg>
	int32_t NotifyPeer(int32_t iInstanceId, const Msg& stMessage)
	{
		const int32_t iThreadStatus = ::OI::Net::Poll::GetInstance().CheckThreadAccess("ProtoMultiClientManager::NotifyPeer");
		if (iThreadStatus != OI_OK)
		{
			return iThreadStatus;
		}
		return PeerCall::StartNotify(PeerSender(iInstanceId), stMessage);
	}
	// 选路同 AnySender；名单为空返回 OI_ERR_CONNECT，返回 OI_OK 只表示任务已启动。
	template <typename Msg>
	int32_t NotifyAny(const Msg& stMessage)
	{
		const int32_t iThreadStatus = ::OI::Net::Poll::GetInstance().CheckThreadAccess("ProtoMultiClientManager::NotifyAny");
		if (iThreadStatus != OI_OK)
		{
			return iThreadStatus;
		}
		return PeerCall::StartNotify(AnySender(), stMessage);
	}
	// 按这一刻的名单给每台各启动一条通知任务，返回启动了几台；线程不对返回错误码。各台失败由任务错误处理器报告。
	template <typename Msg>
	int32_t NotifyAll(const Msg& stMessage)
	{
		const int32_t iThreadStatus = ::OI::Net::Poll::GetInstance().CheckThreadAccess("ProtoMultiClientManager::NotifyAll");
		if (iThreadStatus != OI_OK)
		{
			return iThreadStatus;
		}
		std::vector<int32_t> vIds;
		ServiceRoute stRoute;
		AllSenders(vIds, stRoute);
		return PeerCall::StartBatchNotify(stRoute, stMessage);
	}
	// 只发这些实例，名单里没有的跳过，返回启动了几台；线程不对返回错误码。
	template <typename Msg>
	int32_t NotifyMany(const std::vector<int32_t>& vInstanceIds, const Msg& stMessage)
	{
		const int32_t iThreadStatus = ::OI::Net::Poll::GetInstance().CheckThreadAccess("ProtoMultiClientManager::NotifyMany");
		if (iThreadStatus != OI_OK)
		{
			return iThreadStatus;
		}
		ServiceRoute stRoute;
		ManySenders(vInstanceIds, stRoute);
		return PeerCall::StartBatchNotify(stRoute, stMessage);
	}
	// 指定一台。名单里没有返回 OI_ERR_CONNECT；在名单里还没连上就先暂存，连上补发。返回 TaskResult<int32_t>。
	template <typename Req>
	[[nodiscard]] auto Call(int32_t iInstanceId, Req stRequest, typename RpcProtoId<Req>::Response& stResponse, ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180))
	{
		return PeerCall::RunOutput<Req>(PeerSender(iInstanceId), std::optional<Req>(std::in_place, std::move(stRequest)), stResponse, stTimeout);
	}
	// 任选一台，规则同 AnySender。
	template <typename Req>
	[[nodiscard]] auto CallAny(Req stRequest, typename RpcProtoId<Req>::Response& stResponse, ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180))
	{
		return PeerCall::RunOutput<Req>(AnySender(), std::optional<Req>(std::in_place, std::move(stRequest)), stResponse, stTimeout);
	}
	// 按发送这一刻的名单给每台各发一份，先全部发出再等。结果按实例号升序，带实例号。
	template <typename ExplicitRes = void, typename Req>
	auto CallAll(const Req& stRequest, ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180))
	{
		using Res = typename RpcProtoId<Req>::Response;
		static_assert(std::is_void_v<ExplicitRes> || std::is_same_v<ExplicitRes, Res>, "RPC response type does not match the request");
		std::vector<int32_t> vIds;
		ServiceRoute stRoute;
		AllSenders(vIds, stRoute);
		return PeerCall::RunBatch<Res>(std::move(vIds), std::move(stRoute), stRequest, RpcDeadline(stTimeout));
	}
	// 只打这些实例，先全部发出再等。结果顺序和实例号一致，名单里没有的那一格记 OI_ERR_CONNECT。
	template <typename ExplicitRes = void, typename Req>
	auto CallMany(const std::vector<int32_t>& vInstanceIds, const Req& stRequest, ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180))
	{
		using Res = typename RpcProtoId<Req>::Response;
		static_assert(std::is_void_v<ExplicitRes> || std::is_same_v<ExplicitRes, Res>, "RPC response type does not match the request");
		ServiceRoute stRoute;
		ManySenders(vInstanceIds, stRoute);
		return RouteCall::Send<Res>(std::move(stRoute), stRequest, RpcDeadline(stTimeout), vInstanceIds.size());
	}
};

// 进程内只有这一个路由管理器，无状态调用都走它：调用方只给服务名和分区号，服务端只管登记处理函数。
// 每个服务名一组常驻连接，名单跟着注册表走，共用 SID 空间和断连清理。连接参数取本管理器的配置段 RpcRoute。
class RouteManager final : public ProtoSessionManager
{
	DECLARE_SINGLETON(RouteManager)
	::OI::Thread::Mutex m_stGroupLocker;
	// 组建出来就不删，名单空了只是没有连接。
	std::map<std::string, std::unique_ptr<ServiceGroup>> m_mGroups;
	bool m_bStopped = false;
	std::once_flag m_stStopOnce;
	std::once_flag m_stLimitOnce;

	RouteManager();
	void Stop();
	// 第一次建连接前读 RpcRoute 的发送队列。没配就用基类默认值。
	// 取或建这个名字的组，并按 vList 对齐。停机后返回空。
	ServiceGroup* Group(const std::string& sName, const std::vector<::OI::Base::ServiceInstance>& vList);
	static std::vector<::OI::Base::ServiceInstance> Named(const std::string& sName, const std::vector<::OI::Base::ServiceInstance>& vList);

  public:
	const std::string GetIDString() override;
	void OnAddSession(::OI::Net::Session*) override
	{
	}
	void OnDelSession(const ::OI::Net::Session*) override
	{
	}
	// 按注册表缓存刷新已有的组，下线的实例退役。
	::OI::Coro::Task OnSecondUpdate(time_t iNow);
	ServiceRoute Resolve(const std::string& sName, ServicePick iPick, const std::string& sZone, int32_t iCode, const std::vector<::OI::Base::ServiceInstance>& vList);
	// vClients 与分区号等长，空指针表示这个分区当前没登记。
	ServiceRoute ResolveZones(const std::string& sName, const std::vector<std::string>& vZones, int32_t iCode, const std::vector<::OI::Base::ServiceInstance>& vList);
	// 不是本管理器的连接返回空。
	RouteConnectionInfo ConnectionInfo(const RpcSender& stSender);
	std::shared_ptr<void> ConnectionOwner(const RpcSender& stSender);
	RoutePoolStats Stats();

	// 输出引用版，返回 TaskResult<int32_t>；非 0 时回复保持调用前的值，输出须活到任务结束。
	// 按名字任选一台，优先已连上的。
	template <typename Req>
	[[nodiscard]] auto Call(std::string sName, Req stRequest, typename RpcProtoId<Req>::Response& stResponse, ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180))
	{
		return RouteCall::RunOutput<Req>(std::move(sName), ServicePick::Any, std::string(), std::optional<Req>(std::in_place, std::move(stRequest)), stResponse,
		    stTimeout);
	}
	// 指定分区号，打这个分区里实例号最小的一台。全局服不接受分区号。
	template <typename Req>
	[[nodiscard]] auto Call(std::string sName, int32_t iZoneId, Req stRequest, typename RpcProtoId<Req>::Response& stResponse,
	    ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180))
	{
		if (iZoneId < 0)
		{
			return RouteCall::Fail(OI_ERR_FAIL);
		}
		return RouteCall::RunOutput<Req>(std::move(sName), ServicePick::Zone, std::to_string(iZoneId), std::optional<Req>(std::in_place, std::move(stRequest)),
		    stResponse, stTimeout);
	}
	// 按发送这一刻的名单给每台各发一份，先全部发出再等。结果按实例号升序；选路失败时只有一格。
	template <typename ExplicitRes = void, typename Req>
	auto CallAll(std::string sName, const Req& stRequest, ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180))
	{
		using Res = typename RpcProtoId<Req>::Response;
		static_assert(std::is_void_v<ExplicitRes> || std::is_same_v<ExplicitRes, Res>, "RPC response type does not match the request");
		return RouteCall::Call<Res>(RouteAwaiter::Kind::Named, std::move(sName), ServicePick::All, {}, stRequest, stTimeout);
	}
	// 只打这些分区，结果顺序和分区号一致。没登记的那一格记连接失败。
	template <typename ExplicitRes = void, typename Req>
	auto CallMany(std::string sName, const std::vector<int32_t>& vZoneIds, const Req& stRequest, ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180))
	{
		using Res = typename RpcProtoId<Req>::Response;
		static_assert(std::is_void_v<ExplicitRes> || std::is_same_v<ExplicitRes, Res>, "RPC response type does not match the request");
		return RouteCall::Call<Res>(RouteAwaiter::Kind::Zones, std::move(sName), ServicePick::Any, vZoneIds, stRequest, stTimeout);
	}

	// 单向消息。选路规则分别同上面四个 Call；返回 OI_OK 只表示任务已启动，选路、发送或回执失败由任务错误处理器报告。
	using ProtoSessionManager::Notify;
	template <typename Msg>
	int32_t Notify(std::string sName, const Msg& stMessage)
	{
		return RouteCall::StartNotify(RouteAwaiter::Kind::Named, std::move(sName), ServicePick::Any, std::string(), {}, stMessage);
	}
	// 分区号为负返回 OI_ERR_FAIL，不启动任务。
	template <typename Msg>
	int32_t Notify(std::string sName, int32_t iZoneId, const Msg& stMessage)
	{
		if (iZoneId < 0)
		{
			return OI_ERR_FAIL;
		}
		return RouteCall::StartNotify(RouteAwaiter::Kind::Named, std::move(sName), ServicePick::Zone, std::to_string(iZoneId), {}, stMessage);
	}
	template <typename Msg>
	int32_t NotifyAll(std::string sName, const Msg& stMessage)
	{
		return RouteCall::StartNotify(RouteAwaiter::Kind::Named, std::move(sName), ServicePick::All, std::string(), {}, stMessage);
	}
	// 没登记的分区计入失败，报告里带分区号。
	template <typename Msg>
	int32_t NotifyMany(std::string sName, const std::vector<int32_t>& vZoneIds, const Msg& stMessage)
	{
		return RouteCall::StartNotify(RouteAwaiter::Kind::Zones, std::move(sName), ServicePick::Any, std::string(), vZoneIds, stMessage);
	}
};
}
}
