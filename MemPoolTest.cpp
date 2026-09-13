#include"MemPool.h"
#include<iostream>
#include<vector>
#include<thread>
#include <chrono>
class P1 {
	int id;
};
class P2 {
	int id[10];
};
class P3 {
	int id[25];
};
class P4 {
	int id[40];
};
class P5 {
	int id[2000];
};
template<typename Func>
void RunBenchmark(const std::string& name, size_t nthreads, size_t times, size_t rounds, Func task) {
    std::vector<std::thread> test(nthreads);

    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < nthreads; ++i) {
        test[i] = std::thread([&, task]() {
            for (size_t j = 0; j < times; ++j) {
                for (size_t k = 0; k < rounds; ++k) {
                    task();
                }
            }
            });
    }

    for (auto& t : test) t.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << name << " ºÄÊ±: " << duration << " ms\n";
}

void BenchmarkMemPool(size_t times, size_t nthreads, size_t rounds) {
    RunBenchmark("MemPool (P1-P4)", nthreads, times, rounds, [&]() {
        P1* p1 = mempool::NewElement<P1>(); mempool::DelElement(p1);
        P2* p2 = mempool::NewElement<P2>(); mempool::DelElement(p2);
        P3* p3 = mempool::NewElement<P3>(); mempool::DelElement(p3);
        P4* p4 = mempool::NewElement<P4>(); mempool::DelElement(p4);
        });

    RunBenchmark("System new (P1-P4)", nthreads, times, rounds, [&]() {
        P1* p1 = new P1; delete p1;
        P2* p2 = new P2; delete p2;
        P3* p3 = new P3; delete p3;
        P4* p4 = new P4; delete p4;
        });

    RunBenchmark("MemPool (P5)", nthreads, times, rounds, [&]() {
        P5* p5 = mempool::NewElement<P5>(); mempool::DelElement(p5);
        });

    RunBenchmark("System new (P5)", nthreads, times, rounds, [&]() {
        P5* p5 = new P5; delete p5;
        });
}
int main() {
	mempool::HashMemPool::HashPoolinit();
	BenchmarkMemPool(100, 40, 80);
	return 0;
}