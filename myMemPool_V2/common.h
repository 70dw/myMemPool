#pragma once
#include <cstddef>
#include <atomic>
#include <array>

namespace memoryPool
{
    //constexpr size_t CENTRAL_FETCH_SIZE = 32;
    constexpr size_t ALIGNMENT = 8;
    constexpr size_t MAX_BYTES = 256 * 1024; // 256KB
    constexpr size_t FREE_LIST_SIZE = MAX_BYTES / ALIGNMENT; // ALIGNMENT等于指针void*的大小

    // 内存块头部信息
    struct BlockHeader
    {
        size_t size; // 内存块大小
        bool   inUse; // 使用标志
        BlockHeader* next; // 指向下一个内存块
    };

    // 大小类管理
    class SizeClass
    {
    public:
        static size_t roundUp(size_t bytes)
        {
            //返回不小于bytes的最小alignment的倍数 只在alignment为2的幂成立
            return (bytes + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
        }

        static size_t getIndex(size_t bytes)
        {
            return (bytes + ALIGNMENT - 1) / ALIGNMENT - 1;
        }
    };

} // namespace memoryPool