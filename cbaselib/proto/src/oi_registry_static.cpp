#include "oi_registry.h"
#include "oi_config.h"
#include "oi_errno.h"
#include "oi_log.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace OI
{
namespace Base
{
namespace
{
bool ValidToken(const std::string& sText)
{
	if (sText.empty())
	{
		return false;
	}
	for (const char iCharacter : sText)
	{
		const bool bOk = (iCharacter >= 'a' && iCharacter <= 'z') || (iCharacter >= 'A' && iCharacter <= 'Z') || (iCharacter >= '0' && iCharacter <= '9') || iCharacter == '_' || iCharacter == '-';
		if (!bOk)
		{
			return false;
		}
	}
	return true;
}

bool ParseId(const std::string& sText, int32_t& iId)
{
	if (sText.empty() || sText[0] == '+' || sText[0] == '-')
	{
		return false;
	}
	char* pEnd = nullptr;
	const long iValue = std::strtol(sText.c_str(), &pEnd, 10);
	if (pEnd == nullptr || *pEnd != '\0' || iValue <= 0 || iValue > 2147483647L)
	{
		return false;
	}
	iId = static_cast<int32_t>(iValue);
	return true;
}

std::vector<std::string> SplitFields(const std::string& sLine)
{
	std::vector<std::string> vFields;
	std::string sField;
	for (const char iCharacter : sLine)
	{
		if (iCharacter == ' ' || iCharacter == '\t')
		{
			if (!sField.empty())
			{
				vFields.push_back(sField);
				sField.clear();
			}
			continue;
		}
		sField.push_back(iCharacter);
	}
	if (!sField.empty())
	{
		vFields.push_back(sField);
	}
	return vFields;
}
}

int32_t StaticRegistry::Load()
{
	if (m_bLoaded)
	{
		return m_iStatus;
	}
	m_bLoaded = true;
	std::vector<std::string> vLines;
	if (!::OI::Base::Config::GlobalConfig().GetArrayAs("Registry.instances", vLines))
	{
		m_iStatus = OI_OK;
		return m_iStatus;
	}
	std::vector<ServiceInstance> vInstances;
	for (const std::string& sLine : vLines)
	{
		const std::vector<std::string> vFields = SplitFields(sLine);
		if (vFields.size() != 4 && vFields.size() != 5)
		{
			fprintf(stderr, "registry static bad instance \"%s\"\n", sLine.c_str());
			fflush(stderr);
			LOG_ERROR("registry static bad instance");
			m_iStatus = OI_ERR_INVALID_ARGUMENT;
			return m_iStatus;
		}
		ServiceInstance stInstance;
		stInstance.sName = vFields[0];
		if (!ValidToken(stInstance.sName) || !ParseId(vFields[1], stInstance.iId) || vFields[2].empty() || vFields[3].empty())
		{
			fprintf(stderr, "registry static bad instance \"%s\"\n", sLine.c_str());
			fflush(stderr);
			LOG_ERROR("registry static bad instance");
			m_iStatus = OI_ERR_INVALID_ARGUMENT;
			return m_iStatus;
		}
		stInstance.sIp = vFields[2];
		stInstance.sPort = vFields[3];
		if (vFields.size() == 5)
		{
			stInstance.sZone = vFields[4];
			if (!ValidToken(stInstance.sZone))
			{
				fprintf(stderr, "registry static bad instance \"%s\"\n", sLine.c_str());
				fflush(stderr);
				LOG_ERROR("registry static bad zone");
				m_iStatus = OI_ERR_INVALID_ARGUMENT;
				return m_iStatus;
			}
		}
		for (const ServiceInstance& stHave : vInstances)
		{
			if (stHave.sName == stInstance.sName && stHave.iId == stInstance.iId)
			{
				fprintf(stderr, "registry static duplicate %s id=%d\n", stInstance.sName.c_str(), stInstance.iId);
				fflush(stderr);
				LOG_ERROR("registry static duplicate %s id=%d", stInstance.sName.c_str(), stInstance.iId);
				m_iStatus = OI_ERR_INVALID_ARGUMENT;
				return m_iStatus;
			}
		}
		vInstances.push_back(std::move(stInstance));
	}
	m_vInstances = std::move(vInstances);
	m_iStatus = OI_OK;
	return m_iStatus;
}

bool StaticRegistry::Empty() const
{
	return m_vInstances.empty();
}

void StaticRegistry::Collect(const std::string& sName, std::vector<ServiceInstance>& vList) const
{
	vList.clear();
	for (const ServiceInstance& stInstance : m_vInstances)
	{
		if (stInstance.sName == sName)
		{
			vList.push_back(stInstance);
		}
	}
}

void StaticRegistry::ApplyWatched() const
{
	const std::vector<std::string> vNames = ServiceTable::GetInstance().Names();
	if (vNames.empty())
	{
		return;
	}
	std::vector<ServiceInstance> vList;
	for (const std::string& sName : vNames)
	{
		for (const ServiceInstance& stInstance : m_vInstances)
		{
			if (stInstance.sName == sName)
			{
				vList.push_back(stInstance);
			}
		}
	}
	ServiceTable::GetInstance().Apply(vNames, vList);
}

void StaticRegistry::CheckSelf(const std::string& sName, int32_t iId, const std::string& sIp, const std::string& sPort) const
{
	const ServiceInstance* pFound = nullptr;
	for (const ServiceInstance& stInstance : m_vInstances)
	{
		if (stInstance.sName == sName && stInstance.iId == iId)
		{
			pFound = &stInstance;
			break;
		}
	}
	if (pFound == nullptr)
	{
		fprintf(stderr, "registry static %s id=%d 不在 instances 里\n", sName.c_str(), iId);
		fflush(stderr);
		LOG_ERROR("registry static %s id=%d missing", sName.c_str(), iId);
		return;
	}
	if (pFound->sIp != sIp || pFound->sPort != sPort)
	{
		fprintf(stderr, "registry static %s id=%d 名单 %s:%s 进程 %s:%s\n", sName.c_str(), iId, pFound->sIp.c_str(), pFound->sPort.c_str(), sIp.c_str(), sPort.c_str());
		fflush(stderr);
		LOG_ERROR("registry static %s id=%d list %s:%s process %s:%s", sName.c_str(), iId, pFound->sIp.c_str(), pFound->sPort.c_str(), sIp.c_str(), sPort.c_str());
	}
}
}
}
