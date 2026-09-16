#include "ThreadCache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;

struct Result
{
    std::string name;
    double milliseconds{};
    std::size_t operations{};
};

void touchMemory(void* ptr, std::size_t size, unsigned char value)
{
    if (!ptr)
    {
        throw std::bad_alloc{};
    }
    std::memset(ptr, value, size);
}

void verifyBasicCorrectness(std::size_t count)
{
    constexpr std::size_t blockSize = 64;
    auto* cache = ThreadCache::getInstance();
    std::vector<void*> blocks;
    blocks.reserve(count);
    std::unordered_set<void*> activeAddresses;
    activeAddresses.reserve(count * 2);

    for (std::size_t i = 0; i < count; ++i)
    {
        void* ptr = cache->allocate(blockSize);
        if (!ptr)
        {
            throw std::runtime_error("allocate returned nullptr");
        }
        if (!activeAddresses.insert(ptr).second)
        {
            throw std::runtime_error("the same address was allocated twice while still active");
        }
        touchMemory(ptr, blockSize, static_cast<unsigned char>(i));
        blocks.push_back(ptr);
    }

    for (void* ptr : blocks)
    {
        cache->deallocate(ptr, blockSize);
    }

    blocks.clear();
    activeAddresses.clear();
    for (std::size_t i = 0; i < count; ++i)
    {
        void* ptr = cache->allocate(blockSize);
        if (!ptr || !activeAddresses.insert(ptr).second)
        {
            throw std::runtime_error("reuse correctness check failed");
        }
        touchMemory(ptr, blockSize, 0xA5);
        blocks.push_back(ptr);
    }
    for (void* ptr : blocks)
    {
        cache->deallocate(ptr, blockSize);
    }
}

Result runPoolFixed(std::size_t count, std::size_t size)
{
    auto* cache = ThreadCache::getInstance();
    std::vector<void*> blocks;
    blocks.reserve(count);

    const auto begin = Clock::now();
    for (std::size_t i = 0; i < count; ++i)
    {
        void* ptr = cache->allocate(size);
        touchMemory(ptr, size, static_cast<unsigned char>(i));
        blocks.push_back(ptr);
    }
    for (void* ptr : blocks)
    {
        cache->deallocate(ptr, size);
    }
    const auto end = Clock::now();

    return {"MemoryPool fixed", std::chrono::duration<double, std::milli>(end - begin).count(), count * 2};
}

Result runSystemFixed(std::size_t count, std::size_t size)
{
    std::vector<void*> blocks;
    blocks.reserve(count);

    const auto begin = Clock::now();
    for (std::size_t i = 0; i < count; ++i)
    {
        void* ptr = ::operator new(size);
        touchMemory(ptr, size, static_cast<unsigned char>(i));
        blocks.push_back(ptr);
    }
    for (void* ptr : blocks)
    {
        ::operator delete(ptr);
    }
    const auto end = Clock::now();

    return {"operator new fixed", std::chrono::duration<double, std::milli>(end - begin).count(), count * 2};
}

const std::vector<std::size_t>& mixedSizes()
{
    static const std::vector<std::size_t> sizes{
        8, 16, 24, 32, 48, 64, 96, 128, 256, 512, 1024, 2048
    };
    return sizes;
}

Result runPoolMixed(std::size_t count)
{
    auto* cache = ThreadCache::getInstance();
    std::vector<std::pair<void*, std::size_t>> blocks;
    blocks.reserve(count);
    const auto& sizes = mixedSizes();

    const auto begin = Clock::now();
    for (std::size_t i = 0; i < count; ++i)
    {
        const std::size_t size = sizes[i % sizes.size()];
        void* ptr = cache->allocate(size);
        touchMemory(ptr, size, static_cast<unsigned char>(i));
        blocks.emplace_back(ptr, size);
    }
    for (const auto& [ptr, size] : blocks)
    {
        cache->deallocate(ptr, size);
    }
    const auto end = Clock::now();

    return {"MemoryPool mixed", std::chrono::duration<double, std::milli>(end - begin).count(), count * 2};
}

Result runSystemMixed(std::size_t count)
{
    std::vector<void*> blocks;
    blocks.reserve(count);
    const auto& sizes = mixedSizes();

    const auto begin = Clock::now();
    for (std::size_t i = 0; i < count; ++i)
    {
        const std::size_t size = sizes[i % sizes.size()];
        void* ptr = ::operator new(size);
        touchMemory(ptr, size, static_cast<unsigned char>(i));
        blocks.push_back(ptr);
    }
    for (void* ptr : blocks)
    {
        ::operator delete(ptr);
    }
    const auto end = Clock::now();

    return {"operator new mixed", std::chrono::duration<double, std::milli>(end - begin).count(), count * 2};
}

template <typename Worker>
Result runParallel(const std::string& name, std::size_t threadCount,
                   std::size_t operationsPerThread, Worker worker)
{
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    for (std::size_t threadIndex = 0; threadIndex < threadCount; ++threadIndex)
    {
        threads.emplace_back([&, threadIndex] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            worker(threadIndex, operationsPerThread);
        });
    }

    while (ready.load(std::memory_order_acquire) != threadCount)
    {
        std::this_thread::yield();
    }

    const auto begin = Clock::now();
    start.store(true, std::memory_order_release);
    for (auto& thread : threads)
    {
        thread.join();
    }
    const auto end = Clock::now();

    return {name, std::chrono::duration<double, std::milli>(end - begin).count(),
            threadCount * operationsPerThread * 2};
}

Result runPoolParallel(std::size_t threadCount, std::size_t operationsPerThread)
{
    return runParallel("MemoryPool parallel", threadCount, operationsPerThread,
        [](std::size_t threadIndex, std::size_t count) {
            constexpr std::size_t size = 64;
            auto* cache = ThreadCache::getInstance();
            std::vector<void*> blocks;
            blocks.reserve(count);

            for (std::size_t i = 0; i < count; ++i)
            {
                void* ptr = cache->allocate(size);
                touchMemory(ptr, size, static_cast<unsigned char>(threadIndex + i));
                blocks.push_back(ptr);
            }
            for (void* ptr : blocks)
            {
                cache->deallocate(ptr, size);
            }
        });
}

Result runSystemParallel(std::size_t threadCount, std::size_t operationsPerThread)
{
    return runParallel("operator new parallel", threadCount, operationsPerThread,
        [](std::size_t threadIndex, std::size_t count) {
            constexpr std::size_t size = 64;
            std::vector<void*> blocks;
            blocks.reserve(count);

            for (std::size_t i = 0; i < count; ++i)
            {
                void* ptr = ::operator new(size);
                touchMemory(ptr, size, static_cast<unsigned char>(threadIndex + i));
                blocks.push_back(ptr);
            }
            for (void* ptr : blocks)
            {
                ::operator delete(ptr);
            }
        });
}

void printResult(const Result& result)
{
    const double nanosecondsPerOperation =
        result.milliseconds * 1'000'000.0 / static_cast<double>(result.operations);

    std::cout << std::left << std::setw(25) << result.name
              << std::right << std::setw(12) << std::fixed << std::setprecision(3)
              << result.milliseconds << " ms"
              << std::setw(14) << std::setprecision(1)
              << nanosecondsPerOperation << " ns/op\n";
}
} // namespace

int main(int argc, char* argv[])
{
    const bool quick = argc > 1 && std::string(argv[1]) == "--quick";
    const std::size_t correctnessCount = quick ? 512 : 4'096;
    const std::size_t operationCount = quick ? 10'000 : 100'000;
    const std::size_t threadCount = std::max<std::size_t>(2, std::thread::hardware_concurrency());
    const std::size_t parallelCount = quick ? 2'000 : 20'000;

    try
    {
        std::cout << "Correctness check...\n";
        verifyBasicCorrectness(correctnessCount);
        std::cout << "Correctness check passed.\n\n";

        (void)runPoolFixed(1'000, 64);
        (void)runSystemFixed(1'000, 64);

        std::cout << "Single-thread fixed-size benchmark\n";
        printResult(runPoolFixed(operationCount, 64));
        printResult(runSystemFixed(operationCount, 64));

        std::cout << "\nSingle-thread mixed-size benchmark\n";
        printResult(runPoolMixed(operationCount));
        printResult(runSystemMixed(operationCount));

        std::cout << "\nParallel benchmark (" << threadCount << " threads)\n";
        printResult(runPoolParallel(threadCount, parallelCount));
        printResult(runSystemParallel(threadCount, parallelCount));
    }
    catch (const std::exception& error)
    {
        std::cerr << "Benchmark stopped: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
