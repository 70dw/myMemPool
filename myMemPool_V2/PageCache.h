#pragma once
#include<mutex>
#include<map>
class PageCache {
public:
	struct Span {
		void* pageAddr;
		size_t numPages;
		Span* next;

		//
		size_t blockCount_ = 0;
		size_t freeCount_ = 0;
	};
	static const size_t PAGE_SIZE = 4096;
	static PageCache& getInstance() {
		static PageCache instance;
		return instance;
	}

	Span* allocateSpan(size_t numPages);

	void deallocateSpan(void* ptr, size_t numPages);


private:
	PageCache() = default;
	void* systemAlloc(size_t numPages);

private:
	
	std::map<size_t, Span*>freeSpans_;
	std::map<void*, Span*>spanMap_;
	std::mutex mutex_;


};