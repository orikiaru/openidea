#pragma once
#include "oi_bytes.h"
#include "oi_net_os.h"
namespace OI
{
namespace Net
{
class SessionManager;

class NetAddr
{
	int32_t m_iProtocolType;
	::OI::Base::Bytes m_stAddressData;

  public:
	NetAddr();
	bool Init(SessionManager* pMgr);
	bool IsValidAttr() const;
	sockaddr* NetAttrPtr();
	socklen_t NetAttrLen();
};

class NetSocket
{
	NetAddr m_stAddress;
	net_socket_t m_iSocketDescriptor;
	uint32_t m_iMask;

  public:
	NetSocket();
	virtual ~NetSocket()
	{
	}
	bool Init(net_socket_t iSessionId, SessionManager* pMgr);

	bool IsBind();
	void SetBind();

	bool IsListen();
	void SetListen();

	bool IsConnect();
	void SetConnect();

	bool IsAccept();
	void SetAccept();

	bool IsAsync();
	void SetAsync(bool bAsync);

  public:
	bool CreateSocket(SessionManager* pMgr, bool bAsync);
	bool SocketBind(SessionManager* pMgr);
	bool SocketListen(SessionManager* pMgr);
	bool SocketListenAndBind(SessionManager* pMgr);
	bool SocketConnect(SessionManager* pMgr);
	//accept一个套接字,返回-1失败,返回0说明没有可接受的了
	int SocketAcceptOne(SessionManager* pMgr);
	NetAddr GetNetAddr(SessionManager* pMgr);

  public:
	net_socket_t GetSocket() const
	{
		return m_iSocketDescriptor;
	}
	void Attach(net_socket_t iSessionId)
	{
		m_iSocketDescriptor = iSessionId;
	}
	void Detach()
	{
		m_iSocketDescriptor = INVALID_SOCKET_ID;
	}
	bool IsValidAttr() const
	{
		return m_stAddress.IsValidAttr();
	}
	NetAddr& GetAddr()
	{
		return m_stAddress;
	}
};
}
}