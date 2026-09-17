#pragma once
#include"ThreadCache.h"
#include"PageCache.h"
#include<map>
#include<chrono>
#include<vector>
#include <algorithm>
struct SpanTracker { //沟通中央缓存和线程缓存
	std::atomic<void*>spanAddr{ nullptr }; //申请页的首地址
	std::atomic<size_t>numPages{ 0 };  //一共分了多少页
	std::atomic<size_t>blockCount{ 0 };//一共分出多少块
	std::atomic<size_t>freeCount{ 0 };//如果span中所有块都空闲就还给PageCache
	std::atomic<PageCache::Span*>lordSpan{ nullptr };
};
class CentralCache {
public:
	static CentralCache& getInstance() {
		static CentralCache instance;
		return instance;
	}
	std::pair<void*,size_t> fetchRange(size_t index);
	void returnRange(void* start, size_t numReturn, size_t index);
	

private:
	CentralCache();
	PageCache::Span* fetchFromPageCache(size_t size);

	SpanTracker* getSpanTracker(void* blockAddr);

	void returnToPageCache(SpanTracker* tracker, size_t index);

private:
	std::mutex mutexForTracker_;
	std::array<std::atomic<void*>, FREE_LIST_SIZE>centralFreeList_;

	std::array<std::atomic_flag, FREE_LIST_SIZE>locks_;

	std::map<uintptr_t,SpanTracker>spanTrackers_;
	std::atomic<size_t>spanCount_{ 0 };

	size_t getBatchSize(size_t index)
	{
		constexpr size_t targetBytes = 16 * 1024;
		constexpr size_t minBatch = 4;
		constexpr size_t maxBatch = 256;

		const size_t blockSize = (index + 1) * ALIGNMENT;

		return std::clamp(
			targetBytes / blockSize,
			minBatch,
			maxBatch
		);
	}
};