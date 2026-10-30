#pragma once
#include "oi_lock.h"
#include "oi_header.h"
#include "oi_bytes.h"
#include "oi_net_os.h"
#include "oi_session.h"
#include <map>
#include <string>
namespace OI { namespace Net {

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

class SessionManager
{
protected:
	typedef std::map<sid_t, Session*> SessionMap;
	SessionMap 	_session_map;
	sid_t	   	_cur_session_id;
	::OI::Thread::Mutex   	_locker;
	std::string 	_sip;
	std::string 	_sport;
	SESSION_TYPE	_session_type;		//会话管理器管理的会话类型,TCP,UDP,UNIX,FIFO
	std::string	_service_name;
	std::string	_endpoint;
	bool		_service_down;
	bool		_service_seen;
public:
	SessionManager() :_cur_session_id(0), _session_type(SESSION_TYPE_TCP), _service_down(false), _service_seen(false) {}
	virtual ~SessionManager();
	void DefaultInit(const std::string& sip, const std::string& port, SESSION_TYPE session_type = SESSION_TYPE_TCP)
	{
		_sip = sip; _sport = port; _session_type = session_type;
	}
	const std::string& GetStrIP()	{ return _sip; }
	const std::string& GetStrPort() { return _sport; }
	SESSION_TYPE   GetSessionType() { return _session_type; }
	bool StartClient(bool async = true);	//同步起Client返回值有意义
	bool StartServer(bool async = true);	//目前不支持同步操作
	// 定期拉到的服务列表。online 为假表示这个名字已经从列表里消失。
	void ApplyService(const std::string& ip, const std::string& port, bool online);
	void CloseAllSessions();
	void CloseEndpoint(const std::string& endpoint);
	bool HasEndpoint(const std::string& endpoint);

	bool SendBytes(sid_t sid, const ::OI::Base::Bytes& data);

	static SESSION_TYPE GetSessionTypeByString(const std::string& type);

public:
	//主动关闭会话
	void CloseSession(sid_t sid);

protected:
	friend class NetSession;
	friend class AcceptChannel;
	friend class ConnectChannel;
	friend class NetHelper;
	//AcceptSession调用,必然由IO线程触发
	void AddSession(net_socket_t new_fd);
	void AddSession(Session* pSession);
	//ConnectSession调用,必然有IO线程触发
	void AbortSession();
	//不用怕重入,必然由IO线程触发
	void DelSession(const Session* pSession);

	/*---------------以下是需要重写的内容--------------*/
public:
	virtual bool Init();
	virtual const std::string GetIDString();
protected:
	virtual Session* CloneSession() = 0;//{ 	return new TCPSession(fd, this);	}	//自定制回话类型
	virtual void OnAddSession(Session* pSession) = 0;
	virtual void OnDelSession(const Session* pSession) = 0;
	virtual void OnSessionClosed(sid_t) {}
	virtual void OnConnectFailed() {}
	virtual void OnServiceDown() {}
public:
	// 会话还没建立过时把调用先挂上。返回 false 表示调用方照常发送。
	virtual int32_t UnavailableCode() { return OI_ERR_FAIL; }
	virtual bool TryPendCall(uint64_t, const ::OI::Base::Bytes&) { return false; }
	virtual void DropPendingCall(uint64_t) {}
protected:
};
}}

