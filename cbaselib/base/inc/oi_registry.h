#pragma once
#include "oi_bytes_stream.h"
#include "oi_session_manager.h"
#include "oi_singleton.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace OI
{
namespace Base
{

// 一份在线实例。id 取 [Service] id。
struct ServiceInstance
{
	// 服务名，对应配置里的服务段。
	std::string sName;
	// 实例号，取 [Service] id。同一个名字下选最小的在线 id。
	int32_t iId;
	// 实例地址。
	std::string sIp;
	std::string sPort;
	// 分区。登记时上报，拉取和选路时用来区分同一服务的不同分区。
	std::string sZone;
	ServiceInstance()
	    : iId(0)
	{
	}
};

// registry 和客户端之间的帧：PB 头里是协议号和包体长度。
::OI::Base::Bytes EncodeRegFrame(uint32_t iId, const ::OI::Rpc::Serialize& stMessage);
// 把 in 里完整的帧逐个交给 on_frame，剩下的半帧留在 in。坏帧关掉会话，返回 false。
bool ReadRegFrames(::OI::Base::Bytes& stInput, ::OI::Net::Session* pSession, const std::function<void(uint32_t, ::OI::Base::BytesStream&)>& fnOnFrame);

// 本进程关心的服务。一个名字下面可能有多份实例，选 id 最小的固定走，它下线了再换。
class ServiceTable
{
	DECLARE_SINGLETON(ServiceTable)
	ServiceTable();

  public:
	~ServiceTable();
	// 让这个管理器跟着名字走。列表变化时改它的地址。
	void Watch(::OI::Net::SessionManager* pManager, const std::string& sName);
	// 让这个管理器收这个名字下的整份名单。名单相对 vInitial 有变化时调用 ApplyServiceList。
	void WatchList(::OI::Net::SessionManager* pManager, const std::string& sName, const std::vector<ServiceInstance>& vInitial);
	// 当前关心这个名字的管理器。只含跟着单台切换的，不含收整份名单的。
	std::vector<::OI::Net::SessionManager*> Watchers(const std::string& sName);
	// 正在关心的服务名。
	std::vector<std::string> Names();
	// names 是这次拉取问过的名字，list 里没有的就是下线了。
	void Apply(const std::vector<std::string>& vNames, const std::vector<ServiceInstance>& vList);
	static bool Pick(const std::vector<ServiceInstance>& vList, const std::string& sName, int32_t iCurrent, ServiceInstance& stOutput);

  private:
	struct State
	{
		// 当前选中的实例。同一个名字取 id 最小的在线实例。
		int32_t iId;
		std::string sIp;
		std::string sPort;
		// 最近一次拉取里还在。
		bool bOnline;
		// 关心这个名字的会话管理器。地址变化时通知它们。
		std::vector<::OI::Net::SessionManager*> vWatchers;
		// 上次下发的整份名单，按实例号升序。
		std::vector<ServiceInstance> vList;
		// 收整份名单的会话管理器。
		std::vector<::OI::Net::SessionManager*> vListWatchers;
		// 新订阅者必须收到下一次成功拉取，即使共享名单没有变化。
		std::vector<::OI::Net::SessionManager*> vPendingListWatchers;
		State()
		    : iId(0), bOnline(false)
		{
		}
	};
	// 保护名字表。
	::OI::Thread::Mutex m_stMutex;
	// 服务名到当前选中实例和观察者。
	std::map<std::string, State> m_mServices;
};

struct RegPullState;

struct RegistryPullStats
{
	// 已经发出、还没回的拉取数。
	size_t iSentCount;
	// 还没连上、先记下来的拉取数。
	size_t iDeferredCount;
	// 上面两项之和，不含已完成的回调。
	size_t iLiveCount;
	// 允许同时占住的拉取数。
	int64_t iMaxPendingPulls;
	// 因超时整条连接被作废的次数。
	uint64_t iResetCount;
};

// 连 registry 的客户端。登记、心跳、拉取都在这里，应用层只在配置里写 service。
class RegistryClient : public ::OI::Net::SessionManager
{
	DECLARE_SINGLETON(RegistryClient)
	RegistryClient();

  public:
	virtual ~RegistryClient();
	// 没配 [Registry] 或查不到时返回 false，调用方继续用配置里的 ip/port。
	bool Lookup(const std::string& sName, std::string& sIp, std::string& sPort);
	// 同步拉这个名字下的全部分区。会占住调用线程。协程里用 PullAsync。
	// Lookup、PullSync、Publish 要真正等待时，在 poll 回调里（含定时器回调）调用会抛 logic_error。
	bool PullSync(const std::string& sName, int32_t& iCode, std::vector<ServiceInstance>& vList);
	// 超时毫秒必须为正；默认 1000ms，超时返回 OI_ERR_TIMEOUT，积压满返回 OI_ERR_BUSY。
	// 发出拉取后立刻返回。回包、连不上或超时时调用 fnDone，只调用一次，可能在 poll 线程或定时器线程上。fnDone 里不能再等注册表。
	void PullAsync(const std::string& sName, std::function<void(int32_t, const std::vector<ServiceInstance>&)> fnDone, int64_t iTimeoutMilliseconds = 1000);
	// 未发送和已发送查询的合计占位数，不包含已完成的回调。
	size_t GetPendingPullCount();
	RegistryPullStats GetPullStats();
	// 统一周期扫描；已发送查询过期时失效整条连接，防止 FIFO 回包错配。
	void ExpirePulls();
	// StartServer 成功后登记本进程。返回 false 表示不能起：id 不对，或同名同 id 已经在线。
	bool Publish(const std::string& sSection, const std::string& sIp, const std::string& sPort);
	void Watch(::OI::Net::SessionManager* pManager, const std::string& sName);
	// 收整份名单，vInitial 是调用方已经拿到的那份。新订阅者在下一次成功拉取时收到名单，即使名单未变化。
	// 没配 [Registry] 时什么都不做。
	void WatchList(::OI::Net::SessionManager* pManager, const std::string& sName, const std::vector<ServiceInstance>& vInitial);
	// 按 heartbeat_ms 报一次在线。没连上就去连，连上了还没登记就补登记。
	void Touch();
	// 按 pull_ms 把关心的名字整量拉一次。
	void Pull();
	virtual const std::string GetIDString();
	void OnFrame(sid_t iSessionId, uint32_t iId, ::OI::Base::BytesStream& stBody);

  protected:
	virtual ::OI::Net::Session* CloneSession();
	virtual void OnAddSession(::OI::Net::Session* pSession);
	virtual void OnDelSession(const ::OI::Net::Session* pSession);
	virtual void OnConnectFailed();
	// 配置里有 registry 地址。
	bool Configured();
	// 没连上就去连，然后等到连上或超时。
	bool Ensure();
	// 按配置地址发起连接。
	bool Connect();
	// 等到标志为真、连接失败或超时。单线程靠 PollWait 收包，多线程睡在条件变量上。
	bool WaitFlag(const std::atomic<bool>& bFlag);
	// 唤醒等在条件变量上的同步调用。
	void Notify();
	// 组一帧发给当前会话。没连上返回 false。
	bool Send(uint32_t iId, const ::OI::Rpc::Serialize& stMessage);
	// 发送本进程的登记包。
	void SendRegister();
	// 按心跳间隔挂周期定时器。只挂一次。
	void ArmHeartbeat();
	// 按拉取间隔挂周期定时器。只挂一次。
	void ArmPull();

	// 同步 Lookup、Publish 在这里等连接和回包。
	::OI::Thread::Mutex stMutex;
	::OI::Thread::MutexCond stConditionVariable;
	// 当前 registry 会话可用。
	std::atomic<bool> bConnected;
	// 正在连，避免重复发起。
	std::atomic<bool> bConnecting;
	// 最近一次连接失败。
	std::atomic<bool> bFailed;
	// 当前 registry 会话号。
	std::atomic<sid_t> iSessionId;
	// 配置里的 registry 地址已经读到。
	bool bAddressReady;

	// 本进程要登记的身份。
	std::string sServiceName;
	int32_t iServiceId;
	std::string sServiceIp;
	std::string sServicePort;
	std::string sServiceZone;
	// Publish 已经成功。
	std::atomic<bool> bPublished;
	// 登记回包已经确认。
	std::atomic<bool> bRegistered;
	// 登记之后连接断过，需要补登记。
	std::atomic<bool> bLost;
	// 心跳间隔，毫秒。
	int32_t iHeartbeatMilliseconds;
	// 心跳定时器已经挂上。
	bool bHeartbeatArmed;
	// 拉取间隔，毫秒。
	int32_t iPullMilliseconds;
	// 拉取定时器已经挂上。
	bool bPullArmed;

	// 回包按发出的顺序回来。同步等的那一份在 Lookup / Publish 里等。
	struct PendingPull
	{
		// 真表示 Lookup 或 Publish 在等这一份，回包要叫醒等待线程。
		bool bSynchronous;
		// 超时的单调毫秒。0 表示不单独计时。
		int64_t iDeadlineMilliseconds = 0;
		// 这一次问的服务名。
		std::vector<std::string> vNames;
		// 异步完成回调，和只调用一次的标记。
		std::shared_ptr<RegPullState> pState;
	};
	// 调用方持有 stPendingMutex，且已经连上。发送失败时不留在队列里。
	bool EnqueueSentPull(const PendingPull& stPending);
	bool CanEnqueuePull();
	void ArmPullTimeout();
	// 拉取超时定时器已经挂上。
	bool bPullTimeoutArmed = false;
	// 整条连接因超时被作废的次数。
	uint64_t iPullResetCount = 0;
	// 保护两条拉取队列。
	::OI::Thread::Mutex stPendingMutex;
	// 同时只允许一个同步等待，避免回包叫醒错的调用。
	::OI::Thread::Mutex stSyncMutex;
	// 已发送、按发出顺序等回包。
	std::deque<PendingPull> vPendingPulls;
	// 还没连上，连上再发。
	std::deque<PendingPull> vDeferredPulls;
	// Publish 正在等登记回包。
	std::atomic<bool> bRegistrationWaiting;
	// 同步拉取的回包已经到了。
	std::atomic<bool> bGot;
	// 最近一次同步拉取的返回码。
	int32_t iCode;
	// 最近一次同步拉取的实例列表。
	std::vector<ServiceInstance> vResponseList;
};

}
}
