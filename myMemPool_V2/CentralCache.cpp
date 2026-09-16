#include"CentralCache.h"
#include"PageCache.h"
#include<cstdint>
#include<thread>
#include<cassert>
#include<map>
#include<unordered_map>
#include<iterator>
const std::chrono::milliseconds CentralCache::DELAY_INTERVAL{ 1000 };
static const size_t SPAN_PAGES = 8;
CentralCache::CentralCache() {
	for (auto& ptr : centralFreeList_) {
		ptr.store(nullptr, std::memory_order_relaxed);
	}
	for (auto& lock : locks_) {
		lock.clear();
	}
	for (auto& count : delayCounts_) {
		count.store(0, std::memory_order_relaxed);
	}
	for (auto& time : lastReturnTimes_) {
		time = std::chrono::steady_clock::now();
	}
	spanCount_.store(0, std::memory_order_relaxed);
}

std::pair<void*,size_t> CentralCache::fetchRange(size_t index) {  //需要优化成批量
	size_t giveNum = 0;
	if (index >= FREE_LIST_SIZE)return { nullptr,0 };
	while (locks_[index].test_and_set(std::memory_order_acquire)) {
		std::this_thread::yield();
	}
	void* result = nullptr;
	try {
		result = centralFreeList_[index].load(std::memory_order_relaxed);
		if (!result) {
			size_t size = (index + 1) * ALIGNMENT;
			PageCache::Span* span = fetchFromPageCache(size);
			result = span->pageAddr;
			if (!result) {
				locks_[index].clear(std::memory_order_release);
				return { nullptr,0 };
			}

			char* start = static_cast<char*>(result);
			//实际分配的页数（设定上每次最少分配SPAN_PAGES页）
			size_t numPages = (size <= SPAN_PAGES * PageCache::PAGE_SIZE) ?
				SPAN_PAGES : (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

			//用实际页数计算实际块数
			size_t blockNum = (numPages * PageCache::PAGE_SIZE) / size;
			span->blockCount_ = blockNum;

			void* cur = start;
			for (size_t i = 1;i < blockNum;++i) {
				*reinterpret_cast<void**>(cur) = reinterpret_cast<char*>(cur) + size;
				cur = reinterpret_cast<char*>(cur) + size;;
			}
			*reinterpret_cast<void**>(cur) = nullptr;

			giveNum = std::min(blockNum, getBatchSize(index));
			char* batchTail = start + (giveNum - 1) * size;

			void* centralFreehead = *reinterpret_cast<void**>(batchTail);
			*reinterpret_cast<void**>(batchTail) = nullptr;

			centralFreeList_[index].store(centralFreehead,
				std::memory_order_release
			);
			
			{
				std::lock_guard<std::mutex>guard(mutexForTracker_);
				auto& tracker = spanTrackers_[reinterpret_cast<uintptr_t>(start)];
				tracker.spanAddr.store(start, std::memory_order_release);
				tracker.numPages.store(numPages, std::memory_order_release);
				tracker.blockCount.store(blockNum, std::memory_order_release);
				tracker.freeCount.store(blockNum - giveNum, std::memory_order_release);
				tracker.lordSpan.store(span, std::memory_order_release);
			}

			span->freeCount_ = blockNum - giveNum;
		}
		else {
			void* cur = result;void* pre = nullptr;
			for (;giveNum < getBatchSize(index) && cur;++giveNum) {
				SpanTracker* tracker = getSpanTracker(cur);
				tracker->freeCount.fetch_sub(1, std::memory_order_release);
				tracker->lordSpan.load(std::memory_order_acquire)->freeCount_ --;
				pre = cur;
				cur = *reinterpret_cast<void**>(cur);
			}
			if (pre) {
				*reinterpret_cast<void**>(pre) = nullptr;
				centralFreeList_[index].store(cur, std::memory_order_release);
			}
		}
	}
	catch (...) {
		locks_[index].clear(std::memory_order_release);
		throw;
	}
	locks_[index].clear(std::memory_order_release);
	return { result,giveNum };
}

//归还时 先全还到centralcache的freelist上 如果归还次数过多或时间间隔过久 则把centralfreelist全还给pagecache
void CentralCache::returnRange(void* start, size_t numReturn, size_t index) {
	if (!start || index >= FREE_LIST_SIZE)return;
	size_t blockSize = (index + 1) * ALIGNMENT;
	size_t blockCount = numReturn;
	

	while (locks_[index].test_and_set(std::memory_order_acquire)) {
		std::this_thread::yield();
	}
	try {
		void* end = start;
		SpanTracker* tracker = getSpanTracker(start);
		if (!tracker) {
			throw std::logic_error("SpanTracker not found");
		}
		tracker->lordSpan.load(std::memory_order_acquire)->freeCount_++;
		size_t count = 1;
		while (*reinterpret_cast<void**>(end) && count < blockCount) {
			end = *reinterpret_cast<void**>(end);
			++count;
			SpanTracker* tracker = getSpanTracker(end);
			if (!tracker) {
				throw std::logic_error("SpanTracker not found");
			}
			tracker->lordSpan.load(std::memory_order_acquire)->freeCount_++;
		}
		void* current = centralFreeList_[index].load(std::memory_order_relaxed);
		*reinterpret_cast<void**>(end) = current;
		centralFreeList_[index].store(start, std::memory_order_release);

		size_t currentCount = delayCounts_[index].fetch_add(1, std::memory_order_relaxed) + 1;
		auto currentTime = std::chrono::steady_clock::now();

		if (shouldPerformDelayedReturn(index, currentCount, currentTime)) {
			performDelayedReturn(index);
		}
	}
	catch (...) {
		locks_[index].clear(std::memory_order_release);
		throw;
	}
	locks_[index].clear(std::memory_order_release);
}
//不会立刻归还页内存，而是经过一定时间或者达到次数threshold才检查spantracker，避免刚还了又需要
bool CentralCache::shouldPerformDelayedReturn(size_t index, size_t currentCount, std::chrono::steady_clock::time_point currentTime) {
	if (currentCount >= MAX_DELAY_COUNT)return true;
	auto lastTime = lastReturnTimes_[index];
	return (currentTime - lastTime) >= DELAY_INTERVAL;
}
//把CentralFreeList全部归还 第一步是统计spantracker的free块数
void CentralCache::performDelayedReturn(size_t index) {
	delayCounts_[index].store(0, std::memory_order_relaxed);
	lastReturnTimes_[index] = std::chrono::steady_clock::now();

	std::unordered_map<SpanTracker*, size_t>spanFreeCounts;
	void* currentBlock = centralFreeList_[index].load(std::memory_order_relaxed);

	while (currentBlock) {
		SpanTracker* tracker = getSpanTracker(currentBlock);
		spanFreeCounts[tracker]++;
		currentBlock = *reinterpret_cast<void**>(currentBlock);
	}

	for (const auto& [tracker, newFreeBlocks] : spanFreeCounts) {
		updateSpanFreeCount(tracker, newFreeBlocks, index);
	}
}
//给出tracker和相应free块数 决定是否归还给pagecache操作
void CentralCache::updateSpanFreeCount(SpanTracker* tracker, size_t newFreeBlocks, size_t index) { 
	std::lock_guard<std::mutex>guard(mutexForTracker_);

	tracker->freeCount.store(newFreeBlocks, std::memory_order_release);

	if (newFreeBlocks == tracker->blockCount.load(std::memory_order_relaxed)) {
		
		PageCache::Span* lord = tracker->lordSpan.load(std::memory_order_acquire);
		void* spanAddr = tracker->spanAddr.load(std::memory_order_relaxed);
		size_t numPages = tracker->numPages.load(std::memory_order_relaxed);

		spanTrackers_.erase(reinterpret_cast<uintptr_t>(tracker->spanAddr.load(std::memory_order_relaxed)));

		void* head = centralFreeList_[index].load(std::memory_order_relaxed);
		void* newHead = head; 
		void* prev = nullptr;
		void* current = head;
		//freelist里面属于该span的节点不一定是连续的
		//while里是在重建freelist
		while (current) {
			void* next = *reinterpret_cast<void**>(current);
			if (current >= spanAddr && current < static_cast<char*>(spanAddr) + numPages * PageCache::PAGE_SIZE) {
				if (prev)*reinterpret_cast<void**>(prev) = next;
				else newHead = next;
			}
			else prev = current;
			current = next;
		}
		centralFreeList_[index].store(newHead, std::memory_order_release);
		PageCache::getInstance().deallocateSpan(spanAddr, numPages);

	}
}

PageCache::Span* CentralCache::fetchFromPageCache(size_t size) {
	size_t numPages = (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

	return PageCache::getInstance().allocateSpan(std::max(SPAN_PAGES, numPages));
}
SpanTracker* CentralCache::getSpanTracker(void* blockAddr) {           //优化2：不要线性找
	std::lock_guard<std::mutex>guard(mutexForTracker_);
	uintptr_t aim = reinterpret_cast<uintptr_t>(blockAddr);
	auto it = spanTrackers_.upper_bound(aim);
	if (it != spanTrackers_.begin()) {
		SpanTracker& tracker = prev(it)->second;
		uintptr_t spanstart = prev(it)->first;
		if (aim < spanstart + tracker.numPages.load(std::memory_order_relaxed) * PageCache::PAGE_SIZE)return &tracker;
		else { 
			throw std::logic_error("SpanTracker not found");
			return nullptr; 
		}
	}
	throw std::logic_error("SpanTracker not found");
	return nullptr;
}