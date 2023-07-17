#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h> /* superset of previous */
#include <arpa/inet.h>
#include <unistd.h>
#include "connect_channel.h"
#include "session_manager.h"
#include "logger.h"

#ifndef __UNUSE_OPENSSL__
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

void ConnectChannel::DataIn()
{
	//对于ConnectSession来说,是IO出错了
	//不处理
	if (IsClosed())
	{
		return;
	}
}

void ConnectChannel::DataOut()
{
	if (IsClosed())
	{
		return;
	}

	SESSION_TYPE s_type = _p_session_mgr->GetSessionType();
	switch (s_type)
	{
		case SESSION_TYPE_TCP:
			CheckTcpConnect();
			Close();
			break;
		case SESSION_TYPE_SSL:
			CheckSslConnect();
			break;
		default:
			assert(false);
			break;
	};
}

bool ConnectChannel::TryClose()
{
	if (IsClosed())
	{
		Poll::GetInstance()->IOEventUnRegister(this);
		delete this;
		return true;
	}
	return false;
}

bool ConnectChannel::CheckTcpConnect()
{
	//The  socket  is  nonblocking  and  the connection cannot be completed immediately.  
	//It is possible to select(2) or poll(2) for completion by selecting the socket for writing.  After select(2)indicates writability, 
	//use getsockopt(2) to read the SO_ERROR option at level SOL_SOCKET to determine whether connect() completed successfully (SO_ERROR is zero) or  
	//unsuccessfully  (SO_ERROR is one of the usual error codes listed here, explaining the reason for the failure).
	//       int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen);
	int value = -1;
	bool finish = false;
	socklen_t len = sizeof(value);
	if (getsockopt(_socket.GetSocket(), SOL_SOCKET, SO_ERROR, &value, &len) || value)	//返回值不等于0,或者value不等于0
	{
		//没链接完毕或者被中断等下次触发?
		//暂时先等重连吧
		if (value == EINPROGRESS || value == EINTR)
		{

		}
		_p_session_mgr->AbortSession();
	}
	else
	{
		finish = true;
		_p_session_mgr->AddSession(dup(_socket.GetSocket()));
	}
	//普通tcp连接无论建立成功或失败都关闭连接会话
	return finish;
}

bool ConnectChannel::CheckSslConnect()
{
	bool finish = false;

	return finish;
}

bool ConnectChannel::StartConnect(SessionManager* pMgr, bool async)
{
	SESSION_TYPE s_type = pMgr->GetSessionType();
	switch (s_type)
	{
	case SESSION_TYPE_TCP:
		return StartTcpConnect(pMgr, async);
	case SESSION_TYPE_SSL:
		return StartSslConnect(pMgr, async);
	default:
		printf("ConnectChannel::StartConnect not support session_type=%d\n", s_type);
		assert(false);
	}
	return false;

}

bool ConnectChannel::StartTcpConnect(SessionManager* pMgr, bool async)
{
	NetSocket s;
	if (!s.CreateSocket(pMgr, async))
	{
		return false;
	}

	return s.SocketConnect(pMgr);
}

bool ConnectChannel::StartSslConnect(SessionManager* pMgr, bool async)
{
#ifndef __UNUSE_OPENSSL__
	//初始化
	SSL_library_init();
	OpenSSL_add_all_algorithms();
	SSL_load_error_strings();

	bool verify_peer = false;
	std::string client_ca_path = "";
	std::string client_cert_path = "";
	std::string client_key_path = "";

	SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
	if (!ctx)
	{
		ERR_print_errors_fp(stderr);
		return false;
	}

	if (!verify_peer)
	{
		SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
	}
	else
 	{
		SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
		//加载cafile
		if (SSL_CTX_load_verify_locations(ctx, client_ca_path.c_str(), NULL) != 1)
		{
			ERR_print_errors_fp(stderr);
			SSL_CTX_free(ctx);
			return false;
		}
		//SSL_CTX_use_certificate_chain_file
		if (SSL_CTX_use_certificate_chain_file(ctx, client_cert_path.c_str()) != 1)
		{
			ERR_print_errors_fp(stderr);
			SSL_CTX_free(ctx);
			return false;
		}
		if (SSL_CTX_use_PrivateKey_file(ctx, client_key_path.c_str(), SSL_FILETYPE_PEM) != 1)
		{
			ERR_print_errors_fp(stderr);
			SSL_CTX_free(ctx);
			return false;
		}
		if (!SSL_CTX_check_private_key(ctx))
		{
			ERR_print_errors_fp(stderr);
			SSL_CTX_free(ctx);
			return false;
		}
	}

	NetSocket s;
	if (!s.CreateSocket(pMgr, async))
	{
		return false;
	}

	SSL* ssl = SSL_new(ctx);
	int ret = SSL_set_fd(ssl, s.GetSocket());
	if (ret != 1)
	{
		LOG_ERROR("SSL_set_fd error %s", ERR_lib_error_string(ERR_get_error()));
	}

	SSL_set_connect_state(ssl);

	ret = SSL_do_handshake(ssl);
	if (ret != 1)
	{
		int err = SSL_get_error(ssl, ret);
		if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ)
		{
			Poll::GetInstance()->IOEventRegister(new ConnectChannel(s, pMgr), true, true);
			return true;
		}
		else
		{
			ERR_print_errors_fp(stderr);
			SSL_CTX_free(ctx);
			return false;
		}
	}
	else
	{
		pMgr->AddSession(dup(s.GetSocket()));
	}
#else
	assert(!"UseOpenSSL Must Modify CMakeLists.txt _USE_SSL_ == true to link libssl.so");
#endif
	return false;
}