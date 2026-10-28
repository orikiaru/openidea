#pragma once

// 不进继承链。类里写 DECLARE_SINGLETON(T)，构造函数保持私有。
// 拿实例：T::GetInstance()。
namespace OI { namespace Base {
template<typename T>
struct Singleton
{
	static T& Get()
	{
		static T inst;
		return inst;
	}
};
}}

#define DECLARE_SINGLETON(T) \
	friend struct ::OI::Base::Singleton<T>; \
	T(const T&) = delete; \
	T& operator=(const T&) = delete; \
public: \
	static T& GetInstance() { return ::OI::Base::Singleton<T>::Get(); } \
private:
