#pragma once
#include "oi_bytes_stream.h"
#include "oi_coro.h"
#include "oi_session_manager.h"
#include "oi_singleton.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
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

// 没有 Registry.ip 时用的本地名单。一行一个实例，不连 registry。
class StaticRegistry
{
  public:
	int32_t Load();
	bool Empty() const;
	void Collect(const std::string& sName, std::vector<ServiceInstance>& vList) const;
	void ApplyWatched() const;
	void CheckSelf(const std::string& sName, int32_t iId, const std::string& sIp, const std::string& sPort) const;

  private:
	std::vector<ServiceInstance> m_vInstances;
	bool m_bLoaded = false;
	int32_t m_iStatus = OI_OK;
};

}
}
