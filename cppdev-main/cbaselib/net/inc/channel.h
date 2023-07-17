#pragma once
//Channel��Ӧһ���׽��ֵĶ�ȡ��ʽ,��udp,tcp,ssl,�ܵ�,unix��ȵ�
//��֮��Ե�Session����Ӧ�ò������,����ȡ���������л������л���Ӧ�ò�ʹ��
//Sessionά���Ự��������,Channelֻά��������ʱ���ݺ��׽��ֵ�
#include "spin_lock.h"
#include "net_socket.h"

class Session;
class Channel
{
protected:
	NetSocket	_socket;
	Session*	_p_session;		//�󶨵ĻỰ
	int			_event_mask;	//IO�¼�����
	Mutex		_locker;		//������
public:
	friend class Poll;
	Channel(NetSocket s, Session* pSession) :_socket(s), _p_session(pSession),_event_mask(0) {}
	virtual ~Channel();

public:
	//epoll�¼��ص��ӿ�
	virtual void DataIn() = 0;		//real_data_handle	
	virtual void DataOut() = 0;		//real_data_send
	virtual bool TryClose() = 0;	//check err ? close : nothing done;

public:
	//epoll��ע�¼��޸�
	void AllowSend();
	void ForbidSend();
	void AllowRead();
	void ForbidRead();
};