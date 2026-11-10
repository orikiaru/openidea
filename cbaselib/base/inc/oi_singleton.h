#pragma once
#include <atomic>
#include <cstdint>
#include <ctime>
#include <type_traits>
#include <utility>
#include <vector>
#include "oi_lock.h"

// 不进继承链。类里写 DECLARE_SINGLETON(T)，构造函数保持私有。
// 拿实例：T::GetInstance()。
// 想跟着时间轮更新，就在类里写公有的 OnTickUpdate(uint64_t 毫秒) 或 OnSecondUpdate(time_t 秒)。
// 不写就不注册，相当于空 Update。两者都是 poll 线程上的同步调用，不要挂协程或等待。
namespace OI
{
namespace Base
{
// 单例的心跳和秒更新表。比任何单例都先构造，所以最后析构。
// RunFramework 开始前不分发，main 里的 Init 不会和 Update 交错。
class SingletonUpdates
{
  public:
	typedef void (*TickFunction)(void* pOwner, uint64_t iNowMilliseconds);
	typedef void (*SecondFunction)(void* pOwner, time_t iNow);

	static SingletonUpdates& Instance();

	// 同一个 pOwner 只留一份。任意线程可调。
	void AddTick(void* pOwner, TickFunction fnUpdate);
	void AddSecond(void* pOwner, SecondFunction fnUpdate);
	// 摘掉 pOwner 的回调。单例析构前自动调用。
	void Remove(void* pOwner);

	// RunFramework 进入主循环前打开。
	void Start();
	// 时间轮在 poll 线程调用。回调不持锁执行，回调里新建单例也能注册。
	void DispatchTick(uint64_t iNowMilliseconds);
	void DispatchSecond(time_t iNow);

  private:
	SingletonUpdates() = default;
	SingletonUpdates(const SingletonUpdates&) = delete;
	SingletonUpdates& operator=(const SingletonUpdates&) = delete;

	struct TickEntry
	{
		void* pOwner;
		TickFunction fnUpdate;
	};
	struct SecondEntry
	{
		void* pOwner;
		SecondFunction fnUpdate;
	};
	// 护两张表。分发时只在锁里拷一份。
	::OI::Thread::Mutex m_stMutex;
	std::vector<TickEntry> m_vTicks;
	std::vector<SecondEntry> m_vSeconds;
	std::atomic<bool> m_bStarted{false};
};

template <typename T, typename = void>
struct HasTickUpdate : std::false_type
{
};

template <typename T>
struct HasTickUpdate<T, std::void_t<decltype(std::declval<T&>().OnTickUpdate(uint64_t{}))>> : std::true_type
{
};

template <typename T, typename = void>
struct HasSecondUpdate : std::false_type
{
};

template <typename T>
struct HasSecondUpdate<T, std::void_t<decltype(std::declval<T&>().OnSecondUpdate(time_t{}))>> : std::true_type
{
};

template <typename T>
struct Singleton
{
	// 实例之后构造，所以先于实例析构：析构时先摘回调，再拆实例。
	struct UpdateHook
	{
		T* pInstance;
		explicit UpdateHook(T* pInstanceLocal)
		    : pInstance(pInstanceLocal)
		{
			if constexpr (HasTickUpdate<T>::value)
			{
				SingletonUpdates::Instance().AddTick(pInstance, [](void* pOwner, uint64_t iNowMilliseconds)
				{
					static_cast<T*>(pOwner)->OnTickUpdate(iNowMilliseconds);
				});
			}
			if constexpr (HasSecondUpdate<T>::value)
			{
				SingletonUpdates::Instance().AddSecond(pInstance, [](void* pOwner, time_t iNow)
				{
					static_cast<T*>(pOwner)->OnSecondUpdate(iNow);
				});
			}
		}
		~UpdateHook()
		{
			if constexpr (HasTickUpdate<T>::value || HasSecondUpdate<T>::value)
			{
				SingletonUpdates::Instance().Remove(pInstance);
			}
		}
		UpdateHook(const UpdateHook&) = delete;
		UpdateHook& operator=(const UpdateHook&) = delete;
	};

	static T& Get()
	{
		// 注册表先于实例构造完成，就晚于实例析构。
		(void)SingletonUpdates::Instance();
		static T stInst;
		static UpdateHook stHook(&stInst);
		return stInst;
	}
};
}
}

#define DECLARE_SINGLETON(T)                    \
	friend struct ::OI::Base::Singleton<T>;     \
	T(const T&) = delete;                       \
	T& operator=(const T&) = delete;            \
                                                \
  public:                                       \
	static T& GetInstance()                     \
	{                                           \
		return ::OI::Base::Singleton<T>::Get(); \
	}                                           \
                                                \
  private:
