#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h> /* superset of previous */
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#ifndef __UNUSE_OPENSSL__
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif


#include "accept_channel.h"
#include "session_manager.h"
#include "logger.h"

void AcceptChannel::DataIn()
{
	for (;;)
	{
		int accept_count = _socket.SocketAcceptOne(_p_session_mgr);
		if (accept_count == -1)
		{
			assert(false);
			break;
		}
		if (accept_count == 0)
		{
			break;
		}
	}
}

void AcceptChannel::DataOut()
{
	DataIn();
}

bool AcceptChannel::TryClose()
{
	return false;
}

bool AcceptChannel::StartAccept(SessionManager* pMgr, bool async)
{
	int session_type = pMgr->GetSessionType();
	switch (session_type)
	{
	case SESSION_TYPE_TCP:
		return StartTcpAccept(pMgr, async);
	case SESSION_TYPE_SSL:
		return StartSslAccept(pMgr, async);
	default:
		printf("SessionManager::StartServer not support session_type=%d\n", session_type);
		return false;
	}

}

bool AcceptChannel::StartTcpAccept(SessionManager* pMgr, bool async)
{
	//当同步调用时,线程会被永久性阻塞
	NetSocket s;
	if (!s.CreateSocket(pMgr, async))
	{
		return false;
	}

	if (!s.SocketListenAndBind(pMgr))
	{
		return false;
	}

	if (s.IsAsync())
	{
		AcceptChannel* pChannel = new AcceptChannel(s, pMgr);
		Poll::GetInstance()->IOEventRegister(pChannel, true, true);
	}
	else
	{
		//阻塞就看看怎么办吧,先直接死循环
		while (1)
		{
			int accept_count = s.SocketAcceptOne(pMgr);
			if (accept_count == -1)
			{
				assert(false);
				return false;
			}
		}
	}
	return true;
}

bool AcceptChannel::StartSslAccept(SessionManager* pMgr, bool async)
{
#ifndef __UNUSE_OPENSSL__
	const char* cert_path = "";
	const char* private_key_path = "";
	const char* client_ca_cert_file_path = "";
	const char* client_ca_cert_dir_path = "";
	const char* private_key_password = "";
	SSL_CTX* ctx_ = SSL_CTX_new(TLS_server_method());

	if (!ctx_)
	{
		return false;
	}

	SSL_CTX_set_options(ctx_, SSL_OP_NO_COMPRESSION | SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION);

	SSL_CTX_set_min_proto_version(ctx_, TLS1_1_VERSION);

	// add default password callback before opening encrypted private key
	if (private_key_password != nullptr && (strlen(private_key_password) > 0))
	{
		SSL_CTX_set_default_passwd_cb_userdata(ctx_,
			(char*)private_key_password);
	}

	if (SSL_CTX_use_certificate_chain_file(ctx_, cert_path) != 1)
	{
		SSL_CTX_free(ctx_);
		return false;
	}
	if (SSL_CTX_use_PrivateKey_file(ctx_, private_key_path, SSL_FILETYPE_PEM) !=1)
	{
		SSL_CTX_free(ctx_);
		return false;
	}
	if (client_ca_cert_file_path || client_ca_cert_dir_path)
	{
		SSL_CTX_load_verify_locations(ctx_, client_ca_cert_file_path, client_ca_cert_dir_path);
		SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
	}
#else
	assert(!"UseOpenSSL Must Modify CMakeLists.txt _USE_SSL_ == true to link libssl.so");
#endif
	return false;
}

