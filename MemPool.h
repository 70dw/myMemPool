#pragma once
#include <cstddef>
#include <mutex>
#include <new>
#include <utility>
#include<mutex>
namespace mempool {
#define BLOCK_NUM 64
#define SLOT_INIT_SIZE 8
#define MAX_SLOT_SIZE 512
	struct Slot {
		Slot* next;
	};
	class MemBlock {
		size_t BlockSize_ = 4096;
		size_t SlotSize_;
		Slot* firstBlock_;
		Slot* curSlot_;
		Slot* endSlot_;
		Slot* freeSlot_;

		std::mutex mutexForFreeSlot_;
		std::mutex mutexForNewBlock_;

		void NewBlock(){
			Slot* newBlock = reinterpret_cast<Slot*>(operator new(BlockSize_));
			newBlock->next = firstBlock_;
			firstBlock_ = newBlock;

			size_t pos = reinterpret_cast<size_t>(newBlock);
			size_t rest = (pos + sizeof(Slot*)) % SlotSize_;
			curSlot_ = reinterpret_cast<Slot*>(pos + sizeof(Slot*) + (rest ? SlotSize_ - rest : 0));
			endSlot_ = reinterpret_cast<Slot*>(pos + BlockSize_ - SlotSize_ + 1);
		}
	public:
		void init(size_t SlotSize) {
			SlotSize_ = SlotSize;
			firstBlock_ = curSlot_ = endSlot_ = freeSlot_ = nullptr;
		}
		~MemBlock() {
			Slot* cur = firstBlock_;
			while (cur) {
				Slot* next = cur->next;
				operator delete(reinterpret_cast<void*>(cur));
				cur = next;
			}
		}
		void* allocate() {
			Slot* cur; 
			{
				std::lock_guard<std::mutex>guard(mutexForFreeSlot_);
				if (freeSlot_) {
					cur = freeSlot_;
					freeSlot_ = cur->next;
					return cur;
				}
			}
			std::lock_guard<std::mutex>guard(mutexForNewBlock_);
			if (!curSlot_ || curSlot_ >= endSlot_)NewBlock();
			cur = curSlot_;
			curSlot_ += SlotSize_ / sizeof(Slot);
			return cur;
		}
		void deallocate(void* ptr) {
			std::lock_guard<std::mutex>guard(mutexForFreeSlot_);
			Slot* cur = reinterpret_cast<Slot*>(ptr);
			cur->next = freeSlot_;
			freeSlot_ = cur;
		}
	};
	class HashMemPool {
		inline static MemBlock hashMemPool[BLOCK_NUM];
		static MemBlock& getMemBlock(int id) {
			return hashMemPool[id];
		}
		static void* NewMemory(size_t size) {
			if (size <= 0)return nullptr;
			if (size > MAX_SLOT_SIZE)return operator new(size);
			return getMemBlock((size + SLOT_INIT_SIZE - 1) / SLOT_INIT_SIZE - 1).allocate();
		}
		static void DelMemory(void* ptr, size_t size) {
			if (size <= 0 || !ptr)return;
			if (size > MAX_SLOT_SIZE)operator delete (ptr);
			else getMemBlock((size + SLOT_INIT_SIZE - 1) / SLOT_INIT_SIZE - 1).deallocate(ptr);
		}
	public:
		static void HashPoolinit() {
			for (int i = 0;i < BLOCK_NUM;++i) {
				getMemBlock(i).init((i + 1) * 8);
			}
		}

		template<typename T, typename ...Args>
		friend T* NewElement(Args&&...args);

		template<typename T>
		friend void DelElement(T* p);
		
	};
	template<typename T, typename ...Args>
	T* NewElement(Args&&...args) {
		T* p = nullptr;
		if (p = reinterpret_cast<T*>(HashMemPool::NewMemory(sizeof(T)))) {
			new(p)T(std::forward<Args>(args)...);
		}
		return p;
	}
	template<typename T>
	void DelElement(T* p) {
		if (p) {
			p->~T();
			HashMemPool::DelMemory(reinterpret_cast<void*>(p), sizeof(T));
		}
	}
}