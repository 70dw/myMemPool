#pragma once
#include"common.h"
#include<bitset>
#include<vector>
using namespace memoryPool;
class ThreadCache {
public:
	static ThreadCache* getInstance(){
		thread_local ThreadCache instance;
		return &instance;
	}
	void* allocate(size_t size);
	void deallocate(void* ptr, size_t size);
private:
	ThreadCache() {
		for (int i = 0;i < FREE_LIST_SIZE;++i)maxListSize_[i] = 32 * 1024 / (i + 1);
	}
	~ThreadCache()noexcept;
	//从中心缓存获取内存
	void* fetchFromCentralCache(size_t index);
	//归还内存到中心缓存
	void returnToCentralCache(void* start, size_t index);
	//判断是否需要归还到中心缓存
	bool shouldReturnToCentralCache(size_t index);
	void checkSize();
private:
	//每个线程的自由链表数组
	std::array<void*, FREE_LIST_SIZE>freeList_{};
	std::array<size_t, FREE_LIST_SIZE>freeListSize_{};
	std::array<size_t, FREE_LIST_SIZE>maxListSize_{};
	std::array<size_t, FREE_LIST_SIZE>LastOperator_{};
	std::bitset<FREE_LIST_SIZE>used{};
	std::vector<size_t>usedIndex;

	size_t operatorCount_ = 0;

	static const size_t clearCount_ = 2097151;
	static const size_t INTERVAL = 65535;

	template<typename T,typename ...Args>
	friend T* newElement(Args&& ...args);
	template<typename T>
	friend void delElement(T* ptr);
};