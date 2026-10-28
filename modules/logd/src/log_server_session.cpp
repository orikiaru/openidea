#include "log_server_session.h"
#include "log_frame.h"
#include "log_manager.h"

void LogServerSession::OnDataIn()
{
	const char* data = _ibuff.Begin();
	size_t size = _ibuff.Size();
	size_t used = 0;
	while (size - used >= (size_t)LOG_FRAME_HEAD)
	{
		uint32_t payload = ReadLogFrameLen(data + used);
		if (payload < 1 || payload > (uint32_t)LOG_FRAME_MAX)
		{
			PassiveClose(PARSER_ERR_PROTOCOL_SIZE);
			return;
		}
		if (size - used < (size_t)LOG_FRAME_HEAD + payload)
		{
			break;
		}
		int level = (unsigned char)data[used + LOG_FRAME_HEAD];
		const char* text = data + used + LOG_FRAME_HEAD + 1;
		size_t text_len = payload - 1;
		if (level >= 0 && level < LOG_LEVEL::LOG_LEVEL_COUNT)
		{
			OI::Base::Bytes body(text, text_len);
			LogManager::GetInstance().WriteLog((LOG_LEVEL)level, body);
		}
		used += (size_t)LOG_FRAME_HEAD + payload;
	}
	if (used > 0)
	{
		_ibuff.Erase(0, used);
	}
}
