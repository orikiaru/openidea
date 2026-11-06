#pragma once
#include "oi_bytes.h"
#include "oi_net_os.h"
struct addrinfo;
namespace OI
{
namespace Net
{
class SessionManager;

// IPv4 地址。文本、sockaddr、未指定地址都在这里判断。
class Ipv4Addr
{
	sockaddr_in m_stAddr;

  public:
	Ipv4Addr();
	// 点分文本。不是 IPv4 返回 false。
	bool Init(const std::string& sAddress, uint16_t iPort);
	// 拷进一条 IPv4 sockaddr。族不对返回 false。
	bool Init(const sockaddr* pAddr, socklen_t iLength);
	std::string Host() const;
	uint16_t Port() const;
	// ip:port。
	std::string Endpoint() const;
	bool IsUnspecified() const;
	sockaddr* NetAttrPtr();
	socklen_t NetAttrLen() const;
	// 链表里第一条 IPv4。没有返回空。
	static const addrinfo* Find(const addrinfo* pList);
};

// IPv6 地址。方括号、文本、sockaddr、未指定地址都在这里判断。
class Ipv6Addr
{
	sockaddr_in6 m_stAddr;

  public:
	Ipv6Addr();
	// IPv6 文本。两端方括号会先去掉。不是 IPv6 返回 false。
	bool Init(const std::string& sAddress, uint16_t iPort);
	// 拷进一条 IPv6 sockaddr。族不对返回 false。
	bool Init(const sockaddr* pAddr, socklen_t iLength);
	std::string Host() const;
	uint16_t Port() const;
	// [ip]:port。
	std::string Endpoint() const;
	bool IsUnspecified() const;
	sockaddr* NetAttrPtr();
	socklen_t NetAttrLen() const;
	// 链表里第一条 IPv6。没有返回空。
	static const addrinfo* Find(const addrinfo* pList);
};

// 按地址族选用 Ipv4Addr 或 Ipv6Addr。本身不解析地址。
class NetAddr
{
	// 当前选用的地址族。还没填是 AF_UNSPEC。
	int32_t m_iProtocolType;
	Ipv4Addr m_stV4;
	Ipv6Addr m_stV6;

  public:
	NetAddr();
	// 先试 IPv4，再试 IPv6。都不是返回 false。
	bool Init(const std::string& sIp, const std::string& sPort);
	// 用管理器里的 ip 和端口填地址。
	bool Init(SessionManager* pMgr);
	// 解析结果里有 IPv4 就用 IPv4，否则用 IPv6。
	bool Init(const addrinfo* pList);
	// 拷进一条系统地址，比如 recvfrom 带回来的来源。族不认识返回 false。
	bool Init(const sockaddr* pAddr, socklen_t iLength);
	// AF_INET、AF_INET6，还没填是 AF_UNSPEC。
	int32_t Family() const
	{
		return m_iProtocolType;
	}
	// 不带方括号的地址文本。
	std::string Host() const;
	uint16_t Port() const;
	// 端点文本。交给当前地址族去拼。
	std::string Endpoint() const;
	// 认得出就用 Endpoint()，认不出就按原文字拼 ip:port。
	static std::string Endpoint(const std::string& sIp, const std::string& sPort);
	// 0.0.0.0 或 ::。这种地址不能拿去登记。
	bool IsUnspecified() const;
	// 按当前地址族建 TCP 套接字。
	net_socket_t CreateTcp(bool bNonblock) const;
	// 按当前地址族建 UDP 套接字。
	net_socket_t CreateUdp(bool bNonblock) const;
	// 字面 IPv4 / IPv6 在返回前回调。域名在 poll 线程解析，有 IPv4 用 IPv4，否则用 IPv6。
	// 只在 poll 线程调用。成功失败都回调一次。
	static void Resolve(const std::string& sHost, const std::string& sPort, void (*fnDone)(void* pArgument, bool bOk, NetAddr stAddr), void* pArgument);
	// 地址已经填过。
	bool IsValidAttr() const;
	// 交给系统调用的地址指针。
	sockaddr* NetAttrPtr();
	// 地址长度。
	socklen_t NetAttrLen();

  private:
	// 填字面地址。端口不合法时 bBadPort 为真。域名返回 false，不打日志。
	bool Fill(const std::string& sIp, const std::string& sPort, bool& bBadPort);
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