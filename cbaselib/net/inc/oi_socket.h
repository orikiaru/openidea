#pragma once
#include "oi_bytes.h"
#include "oi_net_os.h"
namespace OI
{
namespace Net
{
class SessionManager;

// 交给 bind / connect 的地址。
class NetAddr
{
	// 协议类型，跟所属管理器一致。
	int32_t m_iProtocolType;
	// 序列化后的 sockaddr。
	::OI::Base::Bytes m_stAddressData;

  public:
	NetAddr();
	// 按管理器的 ip 和端口填好地址。失败返回 false。
	bool Init(SessionManager* pMgr);
	// 地址已经填过。
	bool IsValidAttr() const;
	// 交给系统调用的地址指针。
	sockaddr* NetAttrPtr();
	// 地址长度。
	socklen_t NetAttrLen();
};

// 一个套接字加上它已经做过的 bind / listen / connect。
class NetSocket
{
	// 本套接字使用的地址。
	NetAddr m_stAddress;
	// 系统套接字。无效时是 INVALID_SOCKET_ID。
	net_socket_t m_iSocketDescriptor;
	// 状态位：已初始化、异步、已 bind、已 listen、已 connect、accept 出来的。
	uint32_t m_iMask;

  public:
	NetSocket();
	virtual ~NetSocket()
	{
	}
	// 接过一个已有套接字，并按管理器填地址。
	bool Init(net_socket_t iSessionId, SessionManager* pMgr);

	// 是否已经 bind。重复 SetBind 会断言。
	bool IsBind();
	void SetBind();

	// 是否已经 listen。
	bool IsListen();
	void SetListen();

	// 是否已经发起 connect。
	bool IsConnect();
	void SetConnect();

	// 是否是 accept 出来的连接。
	bool IsAccept();
	void SetAccept();

	// 是否非阻塞。
	bool IsAsync();
	void SetAsync(bool bAsync);

  public:
	// 按管理器的类型新建套接字。bAsync 为真时设成非阻塞。
	bool CreateSocket(SessionManager* pMgr, bool bAsync);
	// 绑到管理器的地址。失败返回 false。
	bool SocketBind(SessionManager* pMgr);
	// 开始监听。还没 bind 时失败。
	bool SocketListen(SessionManager* pMgr);
	// 先 bind 再 listen。
	bool SocketListenAndBind(SessionManager* pMgr);
	// 向管理器的地址发起连接。非阻塞时可能还没连上就返回 true。
	bool SocketConnect(SessionManager* pMgr);
	//accept一个套接字,返回-1失败,返回0说明没有可接受的了
	int SocketAcceptOne(SessionManager* pMgr);
	NetAddr GetNetAddr(SessionManager* pMgr);

  public:
	// 系统套接字号。
	net_socket_t GetSocket() const
	{
		return m_iSocketDescriptor;
	}
	// 接过一个描述符，不关原来的。
	void Attach(net_socket_t iSessionId)
	{
		m_iSocketDescriptor = iSessionId;
	}
	// 只丢掉描述符，不关闭。调用方负责关。
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