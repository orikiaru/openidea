#pragma once
#include "spin_lock.h"
#include "header.h"
#include "session.h"
#include <map>
#include <string>

class Session;
enum SESSION_TYPE
{
	SESSION_TYPE_INVALID,
	SESSION_TYPE_TCP,
	SESSION_TYPE_SSL,
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
	Mutex   	_locker;
	std::string 	_sip;
	std::string 	_sport;
	SESSION_TYPE	_session_type;		//会话管理器管理的会话类型,TCP,UDP,UNIX,FIFO
public:
	SessionManager() :_cur_session_id(0), _session_type(SESSION_TYPE_TCP) {}
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

	bool Send(sid_t sid, const Protocol& p)
	{
		return Send(sid, &p);
	}
	bool Send(sid_t sid, const Protocol* p);

	int BroadCast(const Protocol& p)
	{
		return BroadCast(&p);
	}

	//@return BroadCount
	int BroadCast(const Protocol* p);

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
	void AddSession(const int new_fd);
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
	virtual void OnAbortSession() {}
};

