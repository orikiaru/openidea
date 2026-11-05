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
	std::string sName;
	int32_t iId;
	std::string sIp;
	std::string sPort;
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
	void Watch(::OI::Net::SessionManager* pManager, const std::string& sName);
	std::vector<::OI::Net::SessionManager*> Watchers(const std::string& sName);
	std::vector<std::string> Names();
	// names 是这次拉取问过的名字，list 里没有的就是下线了。
	void Apply(const std::vector<std::string>& vNames, const std::vector<ServiceInstance>& vList);
	static bool Pick(const std::vector<ServiceInstance>& vList, const std::string& sName, int32_t iCurrent, ServiceInstance& stOutput);

  private:
	struct State
	{
		int32_t iId;
		std::string sIp;
		std::string sPort;
		bool bOnline;
		std::vector<::OI::Net::SessionManager*> vWatchers;
		State()
		    : iId(0), bOnline(false)
		{
		}
	};
	::OI::Thread::Mutex m_stMutex;
	std::map<std::string, State> m_mServices;
};

struct RegPullState;

struct RegistryPullStats
{
	size_t iSentCount;
	size_t iDeferredCount;
	size_t iLiveCount;
	int64_t iMaxPendingPulls;
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
	bool Configured();
	bool Ensure();
	bool Connect();
	bool WaitFlag(const std::atomic<bool>& bFlag);
	void Notify();
	bool Send(uint32_t iId, const ::OI::Rpc::Serialize& stMessage);
	void SendRegister();
	void ArmHeartbeat();
	void ArmPull();

	std::mutex stMutex;
	std::condition_variable stConditionVariable;
	std::atomic<bool> bConnected;
	std::atomic<bool> bConnecting;
	std::atomic<bool> bFailed;
	std::atomic<sid_t> iSessionId;
	bool bAddressReady;

	std::string sServiceName;
	int32_t iServiceId;
	std::string sServiceIp;
	std::string sServicePort;
	std::string sServiceZone;
	std::atomic<bool> bPublished;
	std::atomic<bool> bRegistered;
	std::atomic<bool> bLost;
	int32_t iHeartbeatMilliseconds;
	bool bHeartbeatArmed;
	int32_t iPullMilliseconds;
	bool bPullArmed;

	// 回包按发出的顺序回来。同步等的那一份在 Lookup / Publish 里等。
	struct PendingPull
	{
		bool bSynchronous;
		int64_t iDeadlineMilliseconds = 0;
		std::vector<std::string> vNames;
		std::shared_ptr<RegPullState> pState;
	};
	// 调用方持有 stPendingMutex，且已经连上。发送失败时不留在队列里。
	bool EnqueueSentPull(const PendingPull& stPending);
	bool CanEnqueuePull();
	void ArmPullTimeout();
	bool bPullTimeoutArmed = false;
	uint64_t iPullResetCount = 0;
	::OI::Thread::Mutex stPendingMutex;
	::OI::Thread::Mutex stSyncMutex;
	std::deque<PendingPull> vPendingPulls;
	std::deque<PendingPull> vDeferredPulls;
	std::atomic<bool> bRegistrationWaiting;
	std::atomic<bool> bGot;
	int32_t iCode;
	std::vector<ServiceInstance> vResponseList;
};

}
}
