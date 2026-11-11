#pragma once
#include <atomic>
#include <cstdint>
#include <ctime>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>
#include "oi_coro.h"
#include "oi_lock.h"

// 不进继承链。类里写 DECLARE_SINGLETON(T)，构造函数保持私有。
// 拿实例：T::GetInstance()。
// 想跟着时间轮更新，就在类里写公有的 OI::Coro::Task OnTickUpdate(uint64_t 毫秒) 或 OI::Coro::Task OnSecondUpdate(time_t 秒)。
// 不写就不注册，相当于空 Update。每个单例的每种更新各跑在一条独立协程里，里面可以 co_await；
// 上一次没 co_return 之前，这个单例的这种更新不会再启动，期间到点的跳或秒直接跳过，不补。
namespace OI
{
namespace Base
{
// 单例的心跳和秒更新表。比任何单例都先构造，所以最后析构。
// RunFramework 开始前不分发，main 里的 Init 不会和 Update 交错。
class SingletonUpdates
{
  public:
	typedef ::OI::Coro::Task (*TickFunction)(void* pOwner, uint64_t iNowMilliseconds);
	typedef ::OI::Coro::Task (*SecondFunction)(void* pOwner, time_t iNow);

	static SingletonUpdates& Instance();

	// 同一个 pOwner 只留一份。任意线程可调。
	void AddTick(void* pOwner, TickFunction fnUpdate);
	void AddSecond(void* pOwner, SecondFunction fnUpdate);
	// 摘掉 pOwner 的更新，之后不再启动新的。已经在跑的协程不取消。单例析构前自动调用。
	void Remove(void* pOwner);

	// RunFramework 进入主循环前打开。
	void Start();
	// 时间轮在 poll 线程调用。只启动协程，不等它们；表锁外启动，更新里新建单例也能注册。
	void DispatchTick(uint64_t iNowMilliseconds);
	void DispatchSecond(time_t iNow);

  private:
	SingletonUpdates() = default;
	SingletonUpdates(const SingletonUpdates&) = delete;
	SingletonUpdates& operator=(const SingletonUpdates&) = delete;

	// 一个单例的一种更新。在跑的协程持有它，摘表后仍有效。
	struct Entry
	{
		void* pOwner = nullptr;
		TickFunction fnTick = nullptr;
		SecondFunction fnSecond = nullptr;
		// 真表示这条更新的协程还没结束，分发时跳过。
		std::atomic<bool> bRunning{false};
	};
	static ::OI::Coro::Task RunTick(std::shared_ptr<Entry> pEntry, uint64_t iNowMilliseconds);
	static ::OI::Coro::Task RunSecond(std::shared_ptr<Entry> pEntry, time_t iNow);
	// 认领成功才启动；启动失败交还标志并报告。
	template <typename F>
	static void Launch(const std::shared_ptr<Entry>& pEntry, F fnStart);

	// 护两张表。分发时只在锁里拷一份。
	::OI::Thread::Mutex m_stMutex;
	std::vector<std::shared_ptr<Entry>> m_vTicks;
	std::vector<std::shared_ptr<Entry>> m_vSeconds;
	std::atomic<bool> m_bStarted{false};
};

template <typename T, typename = void>
struct HasTickUpdate : std::false_type
{
};

template <typename T>
struct HasTickUpdate<T, std::void_t<decltype(std::declval<T&>().OnTickUpdate(uint64_t{}))>> : std::true_type
{
	static_assert(std::is_same_v<decltype(std::declval<T&>().OnTickUpdate(uint64_t{})), ::OI::Coro::Task>,
	    "OnTickUpdate must return OI::Coro::Task");
};

template <typename T, typename = void>
struct HasSecondUpdate : std::false_type
{
};

template <typename T>
struct HasSecondUpdate<T, std::void_t<decltype(std::declval<T&>().OnSecondUpdate(time_t{}))>> : std::true_type
{
	static_assert(std::is_same_v<decltype(std::declval<T&>().OnSecondUpdate(time_t{})), ::OI::Coro::Task>,
	    "OnSecondUpdate must return OI::Coro::Task");
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
			static_assert(!(HasTickUpdate<T>::value && HasSecondUpdate<T>::value), "a singleton may define only one of OnTickUpdate and OnSecondUpdate");
			if constexpr (HasTickUpdate<T>::value)
			{
				SingletonUpdates::Instance().AddTick(pInstance, [](void* pOwner, uint64_t iNowMilliseconds) -> ::OI::Coro::Task
				{
					return static_cast<T*>(pOwner)->OnTickUpdate(iNowMilliseconds);
				});
			}
			if constexpr (HasSecondUpdate<T>::value)
			{
				SingletonUpdates::Instance().AddSecond(pInstance, [](void* pOwner, time_t iNow) -> ::OI::Coro::Task
				{
					return static_cast<T*>(pOwner)->OnSecondUpdate(iNow);
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
