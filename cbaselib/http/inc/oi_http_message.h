#pragma once
#include "oi_errno.h"
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <memory>
#include <atomic>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Json
{
class Value;
}

namespace OI::Http
{
using Headers = std::vector<std::pair<std::string, std::string>>;

struct FormPart
{
	std::string sName;
	std::string sValue;
	std::optional<std::string> sFilename;
	std::string sContentType;
};

class BodyStream
{
	std::atomic<bool> m_bCancelled{false};

  public:
	virtual ~BodyStream() = default;
	void Cancel() noexcept
	{
		m_bCancelled.store(true);
	}
	bool IsCancelled() const noexcept
	{
		return m_bCancelled.load();
	}
};

class BodySource : public BodyStream
{
  public:
	virtual int32_t Read(std::span<char> vBuffer, size_t& iSize) = 0;
};

class BodySink : public BodyStream
{
  public:
	virtual int32_t Write(std::string_view sBytes) = 0;
};

class QueryParams;

struct Message
{
	Headers vHeaders;
	std::string sBody;
	std::string sContentType;
	std::shared_ptr<BodySource> pBodySource;
	int32_t SetJson(const Json::Value& stValue);
	int32_t GetJson(Json::Value& stValue) const;
	// Include oi_http_json.h for rpcdata and typed containers.
	template <typename T>
	int32_t SetJson(const T& stValue);
	template <typename T>
	int32_t GetJson(T& stValue) const;
	int32_t SetForm(const QueryParams& stForm);
	int32_t GetForm(QueryParams& stForm) const;
	int32_t SetMultipart(const std::vector<FormPart>& vParts);
	int32_t GetMultipart(std::vector<FormPart>& vParts) const;

	const std::string* GetHeader(std::string_view sName) const;
};

struct Cookie
{
	std::string sName;
	std::string sValue;
	std::string sDomain;
	std::string sPath;
	std::string sExpires;
	std::optional<int64_t> iMaxAge;
	bool bSecure = false;
	bool bHttpOnly = false;
	std::string sSameSite;

	static std::string EncodeValue(std::string_view sValue);
	static int32_t DecodeValue(std::string_view sValue, std::string& sOutput);
};

class QueryParams
{
	using Entry = std::pair<std::string, std::string>;
	std::vector<Entry> m_vEntries;

  public:
	using const_iterator = std::vector<Entry>::const_iterator;
	QueryParams() = default;
	QueryParams(std::initializer_list<Entry> vEntries);
	void Add(std::string sName, std::string sValue);
	const std::string* Get(std::string_view sName) const;
	std::vector<std::string> GetAll(std::string_view sName) const;

	// 单值操作命中第一个同名参数。
	const_iterator find(std::string_view sName) const;
	const std::string& at(std::string_view sName) const;
	size_t count(std::string_view sName) const;
	std::string& operator[](std::string sName);
	std::pair<const_iterator, bool> emplace(std::string sName, std::string sValue);
	const_iterator begin() const;
	const_iterator end() const;
};

struct Request : Message
{
	bool bFreshConnection = false;
	std::string sMethod = "GET";
	std::string sPath;
	QueryParams mQuery;
	std::shared_ptr<BodySink> pBodySink;
	int32_t AddCookie(std::string_view sName, std::string_view sValue);
	int32_t GetCookies(std::vector<std::pair<std::string, std::string>>& vOutput) const;
};

struct Response : Message
{
	int32_t iStatus = 0;
	int32_t iCode = OI_ERR_CONNECT;
	std::string sError;
	int32_t AddCookie(const Cookie& stCookie);
	int32_t GetCookies(std::vector<Cookie>& vOutput) const;
};

}
