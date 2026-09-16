#include"PageCache.h"
#include<map>
PageCache::Span* PageCache::allocateSpan(size_t numPages) {
	std::lock_guard<std::mutex>lock(mutex_);

	auto it = freeSpans_.lower_bound(numPages);
	if (it != freeSpans_.end()) {
		Span* span = it->second;
		//span从空闲列表移除
		if (span->next) {
			freeSpans_[it->first] = span->next;
		}
		else freeSpans_.erase(it);

		if (span->numPages > numPages) {
			Span* newSpan = new Span;

			newSpan->pageAddr =
				static_cast<char*>(span->pageAddr)
				+ numPages * PAGE_SIZE;

			newSpan->numPages = span->numPages - numPages;

			spanMap_[newSpan->pageAddr] = newSpan;

			//超出部分放回空闲span列表
			auto& head = freeSpans_[newSpan->numPages];
			newSpan->next = head;
			head = newSpan;

			span->numPages = numPages;
		}

		spanMap_[span->pageAddr] = span;//记录span信息用于回收
		return span;
	}
	void* memory = systemAlloc(numPages);
	//用新的span跟踪和回收
	Span* span = new Span;
	span->pageAddr = memory;
	span->numPages = numPages;
	span->next = nullptr;

	spanMap_[memory] = span;
	return span;
}

void PageCache::deallocateSpan(void* ptr, size_t numPages) {
	std::lock_guard<std::mutex>lock(mutex_);

	auto it = spanMap_.find(ptr);
	if (it == spanMap_.end())return;

	Span* span = it->second;
	char* endAddr = reinterpret_cast<char*>(span->pageAddr) + span->numPages * PAGE_SIZE;
	//尝试合并相邻span 应该用map做，因为相邻页的页数不一定一样，且向左合并时修改首地址 
	//把条件改成是否存在于freespans
	for (auto cur = next(it);cur != spanMap_.end();cur=spanMap_.erase(cur)) {
		if (reinterpret_cast<char*>(cur->second->pageAddr) != endAddr || 
			!freeSpans_.count(cur->second->numPages))break;
		bool found = false;
		Span* head = freeSpans_[cur->second->numPages];
		size_t newPages = 0;
		if (head == cur->second) {
			found = true;
			if (head->next) freeSpans_[cur->second->numPages] = head->next;
			else freeSpans_.erase(cur->second->numPages);
			newPages = head->numPages;
			delete head;
		}
		else {
			while (head->next && head->next != cur->second)head = head->next;
			if (head->next) {
				newPages = head->next->numPages;
				found = true;
				Span* tmp = head->next;
				head->next = tmp->next;
				tmp->next = nullptr;
				delete tmp;
			}
		}
		if (!found)break;
		span->numPages += newPages;
		endAddr += newPages * PAGE_SIZE;
		//eraseSpanList(cur->second);
	}

	auto currentIt = it;
	void* oldAddr = span->pageAddr;

	while (currentIt != spanMap_.begin()) {
		auto leftIt = prev(currentIt);
		Span* leftSpan = leftIt->second;
		if (static_cast<char*>(leftSpan->pageAddr) + leftSpan->numPages * PAGE_SIZE != span->pageAddr ||
			!freeSpans_.count(leftSpan->numPages))break;
		bool found = false;
		auto cur = leftIt;
		Span* head = freeSpans_[cur->second->numPages];
		size_t newPages = 0;
		void* newAddr = nullptr;
		if (head == cur->second) {
			found = true;
			if (head->next) freeSpans_[cur->second->numPages] = head->next;
			else freeSpans_.erase(cur->second->numPages);
			newPages = head->numPages;
			newAddr = head->pageAddr;
			delete head;
		}
		else {
			while (head->next && head->next != cur->second)head = head->next;
			if (head->next) {
				newPages = head->next->numPages;
				newAddr = head->next->pageAddr;
				found = true;
				Span* tmp = head->next;
				head->next = tmp->next;
				tmp->next = nullptr;
				delete tmp;
			}
		}
		if (!found)break;

		spanMap_.erase(leftIt);

		span->pageAddr = newAddr;
		span->numPages += newPages;
	}

	// 左合并改变了起始地址，需要更新 map 的键
	if (span->pageAddr != oldAddr) {
		spanMap_.erase(currentIt);
		spanMap_[span->pageAddr] = span;
	}
	auto& list = freeSpans_[span->numPages];
	span->next = list;
	list = span;
}

void* PageCache::systemAlloc(size_t numPages) {
	return operator new(numPages * PAGE_SIZE);
}

//void PageCache::eraseSpanList(Span* span) {
//	if (!freeSpans_.count(span->numPages))return;
//	auto head = freeSpans_[span->numPages];
//	if (head == span) {
//		if (span->next)freeSpans_[span->numPages] = span->next;
//		else freeSpans_.erase(span->numPages);
//		delete span;
//		return;
//	}
//	while (head->next && head->next != span)head = head->next;
//	if (head->next) {
//		Span* aim = head->next;
//		head->next = aim->next;
//		delete aim;
//	}
//}