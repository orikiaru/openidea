#include "oi_fifo_channel.h"
#include "oi_errno.h"
#include "oi_net_os.h"
#include <cstdio>
#include <uv.h>

namespace OI
{
namespace Net
{
FifoChannel::FifoChannel(Session* pSessionLocal)
    : Channel(NetSocket{}, pSessionLocal)
{
}

FifoChannel::~FifoChannel()
{
	CloseWriter();
}

void FifoChannel::CloseWriter()
{
	NetOs_Close(m_iWriter);
	m_iWriter = INVALID_SOCKET_ID;
}

int32_t FifoChannel::OpenPair()
{
	uv_os_sock_t vSockets[2];
	const int iStatus = uv_socketpair(SOCK_STREAM, 0, vSockets, UV_NONBLOCK_PIPE, UV_NONBLOCK_PIPE);
	if (iStatus != 0)
	{
		fprintf(stderr, "fifo socketpair failed: %s (%d)\n", uv_strerror(iStatus), iStatus);
		return OI_ERR_SOCKET;
	}
	// Windows 的 OVERLAPPED 与非阻塞是两个选项。
	if (NetOs_SetNonblock(vSockets[0]) != 0 || NetOs_SetNonblock(vSockets[1]) != 0)
	{
		fprintf(stderr, "fifo nonblock failed: %d\n", errno);
		NetOs_Close(vSockets[0]);
		NetOs_Close(vSockets[1]);
		return OI_ERR_SOCKET;
	}
	stSocket.Attach(vSockets[0]);
	m_iWriter = vSockets[1];
	return OI_OK;
}

void FifoChannel::Wakeup()
{
	if (m_iWriter == INVALID_SOCKET_ID)
	{
		return;
	}
	int iSent;
	do
	{
		iSent = NetOs_Send(m_iWriter, "w", 1);
	} while (iSent < 0 && errno == EINTR);
	if (iSent != 1 && !(iSent < 0 && errno == EAGAIN))
	{
		fprintf(stderr, "fifo wake failed sent=%d errno=%d\n", iSent, errno);
		CloseWriter(); // EOF 也能唤醒读端。
	}
}

bool FifoChannel::ReadWakeup()
{
	char vWake[256];
	int iRead;
	do
	{
		iRead = NetOs_Recv(stSocket.GetSocket(), vWake, sizeof(vWake));
	} while (iRead == sizeof(vWake) || (iRead < 0 && errno == EINTR));
	if (iRead == 0 || (iRead < 0 && errno != EAGAIN))
	{
		CloseWriter();
		return false;
	}
	return m_iWriter != INVALID_SOCKET_ID;
}

void FifoChannel::DataOut()
{
}

bool FifoChannel::TryClose()
{
	return false;
}

int32_t FifoChannel::AllowSend()
{
	return OI_ERR_INVALID_ARGUMENT;
}
}
}
