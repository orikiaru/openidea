#pragma once
#include "oi_bytes_stream.h"
#include "oi_session_manager.h"
#include "oi_singleton.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace OI
{
namespace Rpc
{
template <typename T>
struct RpcProtoId;
}
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

// 本进程关心的服务。一个名字下面可能有多份实例，选 id 最小的固定走，它下线了再换。
class ServiceTable
{
	DECLARE_SINGLETON(ServiceTable)
	ServiceTable();

  public:
	~ServiceTable();
	// 让这个管理器跟着名字走。列表变化时改它的地址。
	int32_t Watch(::OI::Net::SessionManager* pManager, const std::string& sName);
	// 让这个管理器收这个名字下的整份名单。名单相对 vInitial 有变化时调用 ApplyServiceList。
	int32_t WatchList(::OI::Net::SessionManager* pManager, const std::string& sName, const std::vector<ServiceInstance>& vInitial);
	// 当前关心这个名字的管理器。只含跟着单台切换的，不含收整份名单的。
	std::vector<::OI::Net::SessionManager*> Watchers(const std::string& sName);
	// 周期拉取要带上的服务名：有观察者的，或者查过缓存的。
	std::vector<std::string> Names();
	// names 是这次拉取问过的名字，list 里没有的就是下线了。更新缓存并通知观察者。
	void Apply(const std::vector<std::string>& vNames, const std::vector<ServiceInstance>& vList);
	// 起服同步拉取的结果写进缓存，不通知观察者。vList 里只取这个名字的实例。
	void Store(const std::string& sName, const std::vector<ServiceInstance>& vList);
	// 读缓存，按实例号升序。还没拉到过返回 false，并记下这个名字，之后的周期拉取会带上它。
	bool Snapshot(const std::string& sName, std::vector<ServiceInstance>& vList);
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
		// 拉到过至少一次，vCache 可用。
		bool bKnown;
		// 查过缓存，没有观察者也要周期拉取。
		bool bWanted;
		// 最近一次拉到的这个名字下的全部实例，按实例号升序。
		std::vector<ServiceInstance> vCache;
		// 关心这个名字的会话管理器。地址变化时通知它们。
		std::vector<::OI::Net::SessionManager*> vWatchers;
		// 上次下发的整份名单，按实例号升序。
		std::vector<ServiceInstance> vList;
		// 收整份名单的会话管理器。
		std::vector<::OI::Net::SessionManager*> vListWatchers;
		// 新订阅者必须收到下一次成功拉取，即使共享名单没有变化。
		std::vector<::OI::Net::SessionManager*> vPendingListWatchers;
		State()
		    : iId(0), bOnline(false), bKnown(false), bWanted(false)
		{
		}
	};
	// 保护名字表。
	::OI::Thread::Mutex m_stMutex;
	// 服务名到当前选中实例和观察者。
	std::map<std::string, State> m_mServices;
};

// 连 registry 的客户端。应用层只在配置里写 service。
// 起服时同步登记、同步拉取一次，之后连接、补登记、心跳、整量拉取都由内部 Update 周期处理，结果写进 ServiceTable。
// 运行期选路只读本地缓存，不向 registry 发请求。
class RegistryClient : public ::OI::Net::SessionManager
{
	DECLARE_SINGLETON(RegistryClient)
	RegistryClient();

  public:
	virtual ~RegistryClient();
	// 起服：StartServer 成功后由框架调用，同步登记本进程。无须登记时返回 OI_OK。
	int32_t Publish(const std::string& sSection, const std::string& sIp, const std::string& sPort);
	// 起服：同步强制拉一次这个名字下的全部实例，写进本地缓存，之后由周期拉取维护。会占住调用线程。
	// Publish、PullSync、Lookup 要真正等待时，在 poll 回调里（含定时器回调）调用会抛 logic_error。
	int32_t PullSync(const std::string& sName, std::vector<ServiceInstance>& vList);
	// 起服：PullSync 后按实例号最小选一台。没配 [Registry] 或查不到返回错误码，调用方继续用配置里的 ip/port。
	int32_t Lookup(const std::string& sName, std::string& sIp, std::string& sPort);
	// 运行期：读本地缓存，不发请求。还没拉到过返回 false，名字会加进周期拉取，下一次秒更新就拉。
	bool Snapshot(const std::string& sName, std::vector<ServiceInstance>& vList);
	// 会话管理器跟着这个名字的单台走。没配 [Registry] 时什么都不做。
	int32_t Watch(::OI::Net::SessionManager* pManager, const std::string& sName);
	// 会话管理器收整份名单，vInitial 是它已经拿到的那份。新订阅者在下一次成功拉取时收到名单，即使名单未变化。
	int32_t WatchList(::OI::Net::SessionManager* pManager, const std::string& sName, const std::vector<ServiceInstance>& vInitial);
	// 配置里有 registry 地址。
	bool Configured();
	// 每秒一次，上一次没结束就跳过：没连就连，没登记就登记，到点心跳，到点或被催促时整量拉取。
	::OI::Coro::Task OnSecondUpdate(time_t iNow);
	const std::string GetIDString() override;

  protected:
	::OI::Net::Session* CloneSession() override;
	void OnAddSession(::OI::Net::Session* pSession) override;
	void OnDelSession(const ::OI::Net::Session* pSession) override;
	void OnConnectFailed() override;
	void OnSessionClosed(sid_t iSessionIdLocal) override;

  private:
	// 读心跳和拉取间隔，打开 OnTickUpdate。用过注册表才打开。
	void Activate();
	// 没连上就去连，然后等到连上或超时。
	int32_t Ensure();
	// 按配置地址发起连接。
	int32_t Connect();
	// 等到标志为真、连接失败或超时。单线程靠 PollWait 收包，多线程睡在条件变量上。
	int32_t WaitFlag(const std::atomic<bool>& bFlag);
	int32_t WaitDone(const std::atomic<bool>& bFlag);
	// 唤醒等在条件变量上的同步调用。
	void Notify();
	// 起服同步接口用：发一次带调用号的请求。完成回调只调用一次：回包、超时、发送失败或连接关闭。
	template <typename Req>
	void CallRegistry(const Req& stRequest, int64_t iTimeoutMilliseconds,
	    std::function<void(int32_t, typename ::OI::Rpc::RpcProtoId<Req>::Response&)> fnDone);

	// 同步 Publish、PullSync 在这里等连接和回包。
	::OI::Thread::Mutex m_stMutex;
	::OI::Thread::MutexCond m_stCondition;
	// 保护连接状态切换。持有时不发包、不关会话、不调回调。
	::OI::Thread::Mutex m_stStateMutex;
	::OI::Thread::Mutex m_stPublishMutex;
	std::atomic<bool> m_bConnected;
	std::atomic<bool> m_bConnecting;
	// 最近一次连接失败。
	std::atomic<bool> m_bFailed;
	std::atomic<sid_t> m_iSessionId;
	bool m_bAddressReady;

	// 本进程要登记的身份。
	std::string m_sServiceName;
	int32_t m_iServiceId;
	std::string m_sServiceIp;
	std::string m_sServicePort;
	std::string m_sServiceZone;
	std::atomic<bool> m_bPublished;
	// 登记回包已经确认。
	std::atomic<bool> m_bRegistered;
	// 被别的进程占了，停止登记和心跳。
	std::atomic<bool> m_bLost;

	std::atomic<bool> m_bActive;
	// 有新名字要拉，下一次 OnTickUpdate 不等 pull_ms。
	std::atomic<bool> m_bPullWanted;
	// 刚连上，下一次 OnTickUpdate 马上登记和拉取。
	std::atomic<bool> m_bRefresh;
	// Activate 写完再打开 m_bActive，之后只读。
	int32_t m_iHeartbeatMilliseconds;
	int32_t m_iPullMilliseconds;
	// 只在 OnTickUpdate 里读写，同一时刻只有一条在跑。
	int64_t m_iNextHeartbeatMilliseconds;
	int64_t m_iNextPullMilliseconds;
};

}
}
