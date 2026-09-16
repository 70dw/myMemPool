#include"ThreadCache.h"
#include"CentralCache.h"
#include<cassert>
void* ThreadCache::allocate(size_t size) {
	if (size > MAX_BYTES)return operator new(size);
	if (size == 0)size = ALIGNMENT;
	size_t index = SizeClass::getIndex(size);

	if (void* ptr = freeList_[index]) {
		freeList_[index] = *reinterpret_cast<void**>(ptr);
		freeListSize_[index]--;
		return ptr;
	}
	return fetchFromCentralCache(index);
}

void ThreadCache::deallocate(void* ptr, size_t size) {
	if (!ptr)return;
	if (!size)size = ALIGNMENT;
	if (size > MAX_BYTES) {
		operator delete(ptr);
		return;
	}
	size_t index = SizeClass::getIndex(size);

	*reinterpret_cast<void**>(ptr) = freeList_[index];
	freeList_[index] = ptr;
	++freeListSize_[index];

	if (shouldReturnToCentralCache(index)) {         //当freelist中有很多块时才归还
		returnToCentralCache(freeList_[index], index);
	}
}

bool ThreadCache::shouldReturnToCentralCache(size_t index) {
	constexpr size_t maxCacheBytes = 256 * 1024;
	const size_t blockSize = (index + 1) * ALIGNMENT;
	return freeListSize_[index] > maxCacheBytes / blockSize;
}

void* ThreadCache::fetchFromCentralCache(size_t index) {
	auto [result, giveNum] = CentralCache::getInstance().fetchRange(index);//认为返回的是一块 之后需要优化成批量fetch
	if (giveNum > 1) {
		void* next = *reinterpret_cast<void**>(result);
		*reinterpret_cast<void**>(result) = nullptr;
		freeList_[index] = next;
		freeListSize_[index] = giveNum - 1;
	}
	return result;
}

void ThreadCache::returnToCentralCache(void* start, size_t index) { //把一部分留在freelist，另一部分还给centralcache

	size_t blockSize = (index + 1) * ALIGNMENT;

	size_t batchNum = freeListSize_[index];
	if (batchNum <= 1)return;

	size_t keepNum = std::max(batchNum / 4, size_t(1));
	size_t returnNum = batchNum - keepNum;

	char* current = static_cast<char*>(start);

	char* splitNode = current;
	for (size_t i = 0;i < keepNum - 1;++i) {
		splitNode = reinterpret_cast<char*>(*reinterpret_cast<void**>(splitNode));
		if (!splitNode) {
			assert(false && "free list size mismatch");
			return;
		}
	}
	void* nextNode = *reinterpret_cast<void**>(splitNode);
	*reinterpret_cast<void**>(splitNode) = nullptr;
	freeListSize_[index] = keepNum;
	if (returnNum > 0 && nextNode) {
		CentralCache::getInstance().returnRange(nextNode, returnNum, index);
	}
}