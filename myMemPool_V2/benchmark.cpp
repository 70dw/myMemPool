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

Result runPoolFixed(std::size_t count, std::size_t size,
                    std::size_t roundCount = 1)
{
    auto* cache = ThreadCache::getInstance();
    std::vector<void*> blocks;
    blocks.reserve(count);

    const auto begin = Clock::now();
    for (std::size_t round = 0; round < roundCount; ++round)
    {
        blocks.clear();
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
    }
    const auto end = Clock::now();

    return {"MemoryPool fixed", std::chrono::duration<double, std::milli>(end - begin).count(),
            count * roundCount * 2};
}

Result runSystemFixed(std::size_t count, std::size_t size,
                      std::size_t roundCount = 1)
{
    std::vector<void*> blocks;
    blocks.reserve(count);

    const auto begin = Clock::now();
    for (std::size_t round = 0; round < roundCount; ++round)
    {
        blocks.clear();
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
    }
    const auto end = Clock::now();

    return {"operator new fixed", std::chrono::duration<double, std::milli>(end - begin).count(),
            count * roundCount * 2};
}

const std::vector<std::size_t>& mixedSizes()
{
    static const std::vector<std::size_t> sizes{
        8, 16, 24, 32, 48, 64, 96, 128, 256, 512, 1024, 2048
    };
    return sizes;
}

Result runPoolMixed(std::size_t count, std::size_t roundCount = 1)
{
    auto* cache = ThreadCache::getInstance();
    std::vector<std::pair<void*, std::size_t>> blocks;
    blocks.reserve(count);
    const auto& sizes = mixedSizes();

    const auto begin = Clock::now();
    for (std::size_t round = 0; round < roundCount; ++round)
    {
        blocks.clear();
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
    }
    const auto end = Clock::now();

    return {"MemoryPool mixed", std::chrono::duration<double, std::milli>(end - begin).count(),
            count * roundCount * 2};
}

Result runSystemMixed(std::size_t count, std::size_t roundCount = 1)
{
    std::vector<void*> blocks;
    blocks.reserve(count);
    const auto& sizes = mixedSizes();

    const auto begin = Clock::now();
    for (std::size_t round = 0; round < roundCount; ++round)
    {
        blocks.clear();
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
    }
    const auto end = Clock::now();

    return {"operator new mixed", std::chrono::duration<double, std::milli>(end - begin).count(),
            count * roundCount * 2};
}

template <typename Worker>
Result runParallel(const std::string& name, std::size_t threadCount,
                   std::size_t operationsPerThread, std::size_t roundCount,
                   Worker worker)
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
            worker(threadIndex, operationsPerThread, roundCount);
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
            threadCount * operationsPerThread * roundCount * 2};
}

Result runPoolParallel(std::size_t threadCount, std::size_t operationsPerThread,
                       std::size_t roundCount = 1)
{
    return runParallel("MemoryPool parallel", threadCount, operationsPerThread, roundCount,
        [](std::size_t threadIndex, std::size_t count, std::size_t rounds) {
            constexpr std::size_t size = 64;
            auto* cache = ThreadCache::getInstance();
            std::vector<void*> blocks;
            blocks.reserve(count);

            for (std::size_t round = 0; round < rounds; ++round)
            {
                blocks.clear();
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
            }
        });
}

Result runSystemParallel(std::size_t threadCount, std::size_t operationsPerThread,
                         std::size_t roundCount = 1)
{
    return runParallel("operator new parallel", threadCount, operationsPerThread, roundCount,
        [](std::size_t threadIndex, std::size_t count, std::size_t rounds) {
            constexpr std::size_t size = 64;
            std::vector<void*> blocks;
            blocks.reserve(count);

            for (std::size_t round = 0; round < rounds; ++round)
            {
                blocks.clear();
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

void printSummary(std::vector<Result> results)
{
    std::sort(results.begin(), results.end(),
        [](const Result& left, const Result& right) {
            return left.milliseconds < right.milliseconds;
        });

    const Result& middle = results[results.size() / 2];
    const double nanosecondsPerOperation =
        middle.milliseconds * 1'000'000.0 /
        static_cast<double>(middle.operations);

    std::cout << std::left << std::setw(25) << middle.name
              << std::right << std::setw(12) << std::fixed << std::setprecision(3)
              << middle.milliseconds << " ms"
              << std::setw(14) << std::setprecision(1)
              << nanosecondsPerOperation << " ns/op"
              << "   range [" << std::setprecision(3)
              << results.front().milliseconds << ", "
              << results.back().milliseconds << "] ms\n";
}

template <typename PoolRunner, typename SystemRunner>
void runComparison(const std::string& title, std::size_t sampleCount,
                   PoolRunner poolRunner, SystemRunner systemRunner)
{
    std::vector<Result> poolResults;
    std::vector<Result> systemResults;
    poolResults.reserve(sampleCount);
    systemResults.reserve(sampleCount);

    for (std::size_t sample = 0; sample < sampleCount; ++sample)
    {
        if (sample % 2 == 0)
        {
            poolResults.push_back(poolRunner());
            systemResults.push_back(systemRunner());
        }
        else
        {
            systemResults.push_back(systemRunner());
            poolResults.push_back(poolRunner());
        }
    }

    std::cout << title << " (median of " << sampleCount << " samples)\n";
    printSummary(std::move(poolResults));
    printSummary(std::move(systemResults));
}
} // namespace

int main(int argc, char* argv[])
{
    const bool quick = argc > 1 && std::string(argv[1]) == "--quick";
    const std::size_t correctnessCount = quick ? 512 : 4'096;
    const std::size_t operationCount = quick ? 10'000 : 100'000;
    const std::size_t threadCount = std::max<std::size_t>(2, std::thread::hardware_concurrency());
    const std::size_t parallelCount = quick ? 2'000 : 20'000;
    // The previous fixed-size benchmark took roughly 9-11 ms per round.
    // 64 rounds keeps even that fastest case above about 500 ms per sample,
    // which reduces timer noise and short-lived system effects.
    const std::size_t roundCount = quick ? 1 : 64;
    const std::size_t sampleCount = quick ? 1 : 5;

    try
    {
        std::cout << "Correctness check...\n";
        verifyBasicCorrectness(correctnessCount);
        std::cout << "Correctness check passed.\n\n";

        (void)runPoolFixed(1'000, 64);
        (void)runSystemFixed(1'000, 64);

        std::cout << "Rounds per sample: " << roundCount
                  << ", samples: " << sampleCount << "\n\n";

        runComparison("Single-thread fixed-size benchmark", sampleCount,
            [&] { return runPoolFixed(operationCount, 64, roundCount); },
            [&] { return runSystemFixed(operationCount, 64, roundCount); });

        std::cout << '\n';
        (void)runPoolMixed(1'000);
        (void)runSystemMixed(1'000);
        runComparison("Single-thread mixed-size benchmark", sampleCount,
            [&] { return runPoolMixed(operationCount, roundCount); },
            [&] { return runSystemMixed(operationCount, roundCount); });

        std::cout << '\n';
        (void)runPoolParallel(threadCount, 1'000);
        (void)runSystemParallel(threadCount, 1'000);
        runComparison("Parallel benchmark (" + std::to_string(threadCount) + " threads)",
            sampleCount,
            [&] { return runPoolParallel(threadCount, parallelCount, roundCount); },
            [&] { return runSystemParallel(threadCount, parallelCount, roundCount); });
    }
    catch (const std::exception& error)
    {
        std::cerr << "Benchmark stopped: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
