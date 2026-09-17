#pragma once
#include"common.h"
using namespace memoryPool;
class ThreadCache {
public:
	static ThreadCache* getInstance(){
		static thread_local ThreadCache instance;
		return &instance;
	}
	void* allocate(size_t size);
	void deallocate(void* ptr, size_t size);
private:
	ThreadCache() = default;
	~ThreadCache();
	//从中心缓存获取内存
	void* fetchFromCentralCache(size_t index);
	//归还内存到中心缓存
	void returnToCentralCache(void* start, size_t index);
	//判断是否需要归还到中心缓存
	bool shouldReturnToCentralCache(size_t index);
private:
	//每个线程的自由链表数组
	std::array<void*, FREE_LIST_SIZE>freeList_{};
	std::array<size_t, FREE_LIST_SIZE>freeListSize_{};

	template<typename T,typename ...Args>
	friend T* newElement(Args&& ...args);
	template<typename T>
	friend void delElement(T* ptr);
};