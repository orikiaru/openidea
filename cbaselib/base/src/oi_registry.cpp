#include "oi_registry.h"
#include "oi_poll.h"
#include <algorithm>

namespace OI
{
namespace Base
{

namespace
{
std::vector<ServiceInstance> MatchSorted(const std::vector<ServiceInstance>& vList, const std::string& sName)
{
	std::vector<ServiceInstance> vMatched;
	for (const auto& stInst : vList)
	{
		if (stInst.sName == sName)
		{
			vMatched.push_back(stInst);
		}
	}
	std::sort(vMatched.begin(), vMatched.end(), [](const ServiceInstance& stLeft, const ServiceInstance& stRight)
	{
		return stLeft.iId < stRight.iId;
	});
	return vMatched;
}
}

ServiceTable::ServiceTable()
{
}
ServiceTable::~ServiceTable()
{
}

bool ServiceTable::Pick(const std::vector<ServiceInstance>& vList, const std::string& sNameLocal, int32_t iCurrent, ServiceInstance& stOutput)
{
	const ServiceInstance* pBest = NULL;
	for (size_t iIndexLocal = 0; iIndexLocal < vList.size(); ++iIndexLocal)
	{
		if (vList[iIndexLocal].sName != sNameLocal)
		{
			continue;
		}
		if (iCurrent > 0 && vList[iIndexLocal].iId == iCurrent)
		{
			stOutput = vList[iIndexLocal];
			return true;
		}
		if (!pBest || vList[iIndexLocal].iId < pBest->iId)
		{
			pBest = &vList[iIndexLocal];
		}
	}
	if (!pBest)
	{
		return false;
	}
	stOutput = *pBest;
	return true;
}

std::vector<::OI::Net::SessionManager*> ServiceTable::Watchers(const std::string& sNameLocal)
{
	std::vector<::OI::Net::SessionManager*> vWatchers;
	::OI::Thread::MutexGuard stLock(m_stMutex);
	auto stIterator = m_mServices.find(sNameLocal);
	if (stIterator == m_mServices.end())
	{
		return vWatchers;
	}
	vWatchers = stIterator->second.vWatchers;
	return vWatchers;
}

int32_t ServiceTable::Watch(::OI::Net::SessionManager* pManager, const std::string& sNameLocal)
{
	const int32_t iThreadStatus = ::OI::Net::Poll::GetInstance().CheckThreadAccess("ServiceTable::Watch");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}
	if (!pManager || sNameLocal.empty())
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	::OI::Thread::MutexGuard stLock(m_stMutex);
	State& stState = m_mServices[sNameLocal];
	for (size_t iIndexLocal = 0; iIndexLocal < stState.vWatchers.size(); ++iIndexLocal)
	{
		if (stState.vWatchers[iIndexLocal] == pManager)
		{
			return OI_OK;
		}
	}
	stState.vWatchers.push_back(pManager);
	return OI_OK;
}

int32_t ServiceTable::WatchList(::OI::Net::SessionManager* pManager, const std::string& sNameLocal, const std::vector<ServiceInstance>& vInitial)
{
	const int32_t iThreadStatus = ::OI::Net::Poll::GetInstance().CheckThreadAccess("ServiceTable::WatchList");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}
	if (!pManager || sNameLocal.empty())
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	::OI::Thread::MutexGuard stLock(m_stMutex);
	State& stState = m_mServices[sNameLocal];
	for (const auto* pWatcher : stState.vListWatchers)
	{
		if (pWatcher == pManager)
		{
			return OI_OK;
		}
	}
	// 已有别的管理器在收时保留上次下发的名单，免得它漏掉这次之后的变化。
	if (stState.vListWatchers.empty())
	{
		stState.vList = vInitial;
		std::sort(stState.vList.begin(), stState.vList.end(), [](const ServiceInstance& stLeft, const ServiceInstance& stRight)
		{
			return stLeft.iId < stRight.iId;
		});
	}
	stState.vListWatchers.push_back(pManager);
	stState.vPendingListWatchers.push_back(pManager);
	return OI_OK;
}

std::vector<std::string> ServiceTable::Names()
{
	std::vector<std::string> vNamesLocal;
	::OI::Thread::MutexGuard stLock(m_stMutex);
	for (const auto& [sName, stState] : m_mServices)
	{
		if (stState.bWanted || !stState.vWatchers.empty() || !stState.vListWatchers.empty())
		{
			vNamesLocal.push_back(sName);
		}
	}
	return vNamesLocal;
}

void ServiceTable::Store(const std::string& sName, const std::vector<ServiceInstance>& vList)
{
	std::vector<ServiceInstance> vMatched = MatchSorted(vList, sName);
	::OI::Thread::MutexGuard stLock(m_stMutex);
	State& stState = m_mServices[sName];
	stState.vCache.swap(vMatched);
	stState.bKnown = true;
	stState.bWanted = true;
}

bool ServiceTable::Snapshot(const std::string& sName, std::vector<ServiceInstance>& vList)
{
	::OI::Thread::MutexGuard stLock(m_stMutex);
	State& stState = m_mServices[sName];
	stState.bWanted = true;
	if (!stState.bKnown)
	{
		return false;
	}
	vList = stState.vCache;
	return true;
}

void ServiceTable::Apply(const std::vector<std::string>& vNames, const std::vector<ServiceInstance>& vList)
{
	struct Change
	{
		::OI::Net::SessionManager* pManager;
		std::string sIp;
		std::string sPort;
		bool bOnline;
	};
	struct ListChange
	{
		::OI::Net::SessionManager* pManager;
		std::vector<ServiceInstance> vList;
	};
	std::vector<Change> vChanged;
	std::vector<ListChange> vListChanged;
	{
		::OI::Thread::MutexGuard stLock(m_stMutex);
		for (size_t iNumber = 0; iNumber < vNames.size(); ++iNumber)
		{
			auto stIterator = m_mServices.find(vNames[iNumber]);
			if (stIterator == m_mServices.end())
			{
				continue;
			}
			State& stState = stIterator->second;
			std::vector<ServiceInstance> vMatched = MatchSorted(vList, vNames[iNumber]);
			stState.vCache = vMatched;
			stState.bKnown = true;
			if (!stState.vListWatchers.empty())
			{
				const bool bSame = vMatched.size() == stState.vList.size() &&
				                   std::equal(vMatched.begin(), vMatched.end(), stState.vList.begin(), [](const ServiceInstance& stLeft, const ServiceInstance& stRight)
				{
					return stLeft.iId == stRight.iId && stLeft.sIp == stRight.sIp && stLeft.sPort == stRight.sPort && stLeft.sZone == stRight.sZone;
				});
				if (!bSame)
				{
					stState.vList = vMatched;
					for (auto* pWatcher : stState.vListWatchers)
					{
						vListChanged.push_back(ListChange{pWatcher, vMatched});
					}
				}
				else
				{
					for (auto* pWatcher : stState.vPendingListWatchers)
					{
						vListChanged.push_back(ListChange{pWatcher, vMatched});
					}
				}
				stState.vPendingListWatchers.clear();
			}
			ServiceInstance stPick;
			bool bOnlineLocal = Pick(vList, vNames[iNumber], stState.bOnline ? stState.iId : 0, stPick);
			if (bOnlineLocal == stState.bOnline && (!bOnlineLocal || (stPick.iId == stState.iId && stPick.sIp == stState.sIp && stPick.sPort == stState.sPort)))
			{
				continue;
			}
			stState.bOnline = bOnlineLocal;
			stState.iId = bOnlineLocal ? stPick.iId : 0;
			stState.sIp = bOnlineLocal ? stPick.sIp : "";
			stState.sPort = bOnlineLocal ? stPick.sPort : "";
			for (size_t iIndexLocal = 0; iIndexLocal < stState.vWatchers.size(); ++iIndexLocal)
			{
				Change stCharacter;
				stCharacter.pManager = stState.vWatchers[iIndexLocal];
				stCharacter.sIp = stState.sIp;
				stCharacter.sPort = stState.sPort;
				stCharacter.bOnline = bOnlineLocal;
				vChanged.push_back(stCharacter);
			}
		}
	}
	for (size_t iIndexLocal = 0; iIndexLocal < vChanged.size(); ++iIndexLocal)
	{
		vChanged[iIndexLocal].pManager->ApplyService(vChanged[iIndexLocal].sIp, vChanged[iIndexLocal].sPort, vChanged[iIndexLocal].bOnline);
	}
	for (const auto& stChange : vListChanged)
	{
		stChange.pManager->ApplyServiceList(stChange.vList);
	}
}

}
}
