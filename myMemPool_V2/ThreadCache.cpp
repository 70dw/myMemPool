#include"ThreadCache.h"
#include"CentralCache.h"
#include<cassert>
#include<vector>
void* ThreadCache::allocate(size_t size) {
	++operatorCount_;
	if (!(operatorCount_ & clearCount_)) {
		checkSize();
	}
	if (size > MAX_BYTES)return operator new(size);
	if (size == 0)size = ALIGNMENT;
	size_t index = SizeClass::getIndex(size);
	LastOperator_[index] = operatorCount_;
	if (!used[index]) {
		used[index] = 1;
		usedIndex.push_back(index);
	}
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

	if (shouldReturnToCentralCache(index)) {         //当freelist中空间达到阈值时才归还 且增大maxSize
		maxListSize_[index] *= 2;
		returnToCentralCache(freeList_[index], index);
	}
}

bool ThreadCache::shouldReturnToCentralCache(size_t index) {
	const size_t blockSize = (index + 1) * ALIGNMENT;
	return freeListSize_[index] > maxListSize_[index];
}

void* ThreadCache::fetchFromCentralCache(size_t index) {
	auto [result, giveNum] = CentralCache::getInstance().fetchRange(index);
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

	size_t keepTarget = std::max(batchNum / 4, size_t(1));
	size_t returnNum = std::min(size_t(512), batchNum - keepTarget);  //减少独占锁的时间
	size_t keepNum = batchNum - returnNum;

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
template<typename T, typename ...Args>
T* newElement(Args&& ...args) {
	void* p = allocate(sizeof(T));
	if (!p) {
		throw std::logic_error("new room fail!");
	}
	new(p) T(std::forward<Args>(args)...);
	return p;
}
template<typename T>
void delElement(T* ptr) {
	if (!ptr) {
		throw std::logic_error("delete nullptr!");
	}
	T p = *ptr;
	~p();
	deallocate(reinterpret_cast<void*>(ptr), sizeof(T));
}
ThreadCache::~ThreadCache() noexcept{
	for (size_t index = 0;index < FREE_LIST_SIZE;++index){
		void* head = freeList_[index];
		const size_t count = freeListSize_[index];

		if (!head){
			assert(count == 0);
			continue;
		}

		assert(count > 0);

		// 先解除 ThreadCache 对链表的持有
		freeList_[index] = nullptr;
		freeListSize_[index] = 0;

		try{
			CentralCache::getInstance().returnRange(head, count, index);
		}
		catch (...){
			std::terminate();
		}
	}
}

void ThreadCache::checkSize() {
	for (size_t& i : usedIndex) {
		if (operatorCount_ - LastOperator_[i] >= INTERVAL && maxListSize_[i] * (i + 1) > 32 * 1024) {
			maxListSize_[i] /= 2;
			while (freeListSize_[i] > maxListSize_[i]) {
				returnToCentralCache(freeList_[i], i);
			}
		}
	}
}