#pragma once
#include "bytes.h"
#include <vector>
#include <list>
#include <map>
#include <string>

struct ParserException
{
	const char* what() const throw()
	{
		return "ParserException";
	}
};

class Serialize;
class BytesStream :public Bytes
{
	size_t 	_cur_pos;	//反序列化时使用
	size_t  _snapshot_pos;	//快照位置
public:
	BytesStream() :_cur_pos(0), _snapshot_pos(0) {}
	BytesStream(const Bytes& bytes) :Bytes(bytes), _cur_pos(0), _snapshot_pos(0) {}
	BytesStream(const char* data, size_t data_len) :Bytes(data, data_len), _cur_pos(0), _snapshot_pos(0) {}
	Bytes& GetMutableBuff() { return *this; }
	const Bytes& GetUnMutableBuff()const { return *this; }
	size_t	GetCurPos()const { return _cur_pos; }
	void 	AddCurPos(size_t offset) { _cur_pos += offset; }
	char*	GetCurPtr() { return &(Begin()[_cur_pos]); }
	size_t	RemainSize()const { return Size() - _cur_pos; }

	void	StartTransaction() { _snapshot_pos = _cur_pos; }
	void  	RollBack() { _cur_pos = _snapshot_pos; }

	BytesStream& operator<< (const char& a);
	BytesStream& operator<< (const short& a);
	BytesStream& operator<< (const int& a);
	BytesStream& operator<< (const uint32_t& a);
	BytesStream& operator<< (const int64_t& a);
	BytesStream& operator<< (const float& a);
	BytesStream& operator<< (const double& a);
	BytesStream& operator<< (const std::string& a);
	BytesStream& operator<< (const Bytes& a);
	BytesStream& operator<< (const BytesStream& a);	//单纯用来方便流化的,全量buff流入,内部已经是序列化好的内容了

	BytesStream& operator<< (const Serialize& a);

	BytesStream& operator>> (char& a);
	BytesStream& operator>> (short& a);
	BytesStream& operator>> (int& a);
	BytesStream& operator>> (uint32_t& a);
	BytesStream& operator>> (int64_t& a);
	BytesStream& operator>> (float& a);
	BytesStream& operator>> (double& a);
	BytesStream& operator>> (std::string& a);
	BytesStream& operator>> (Bytes& a);

	BytesStream& operator>> (Serialize& a);

	template<typename T>
	BytesStream& operator>> (std::vector<T>& t)
	{
		uint32_t size;
		(*this) >> size;
		t.resize(size);
		for (uint32_t i = 0; i < size; ++i)
		{
			(*this) >> t[i];
		}
		return *this;
	}
	template<typename T>
	BytesStream& operator << (const std::vector<T>& t)
	{
		uint32_t size = t.size();
		(*this) << size;
		for (typename std::vector<T>::const_iterator it = t.begin(); it != t.end(); ++it)
		{
			(*this) << *it;
		}
		return *this;
	}

	/*
	template<typename Key, typename Value>
	BytesStream& operator>> (std::map<Key, Value>& t)
	{

	}

	template<typename Key, typename Value>
	BytesStream& operator<< (std::map<Key, Value>& t)
	{

	}	
	*/




};
