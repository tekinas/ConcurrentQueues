#include "ComputeCallbackGenerator.h"
#include "Parse.h"
#include "SpinLock.h"
#include "timer.hpp"
#include <RingBuffers/FunctionQueue.h>
#include <RingBuffers/FunctionQueueMCSP.h>
#include <RingBuffers/FunctionQueueSCSP.h>
#include <fmt/format.h>
#include <latch>
#include <mutex>
#include <ranges>
#include <thread>

using ComputeFunctionSig = size_t(size_t);
using FQUS = rb::FunctionQueue<ComputeFunctionSig, rb::FQOpt::InvokeOnce>;
using FQSCSP = rb::FunctionQueueSCSP<ComputeFunctionSig, rb::FQOpt::InvokeOnce, false>;
using FQMCSP = rb::FunctionQueueMCSP<ComputeFunctionSig, rb::FQOpt::InvokeOnce, false>;

template<typename FQ>
void reader(FQ &fq, size_t seed, size_t t, size_t readers, std::latch &start_latch, std::vector<size_t> &result_vector,
            std::mutex &res_vec_mut) {
    std::vector<size_t> res_vec;
    auto const N = fq.count() / readers + 1;
    res_vec.reserve(N);
    auto consume_func = [&](auto func) { res_vec.push_back(func(seed)); };
    start_latch.arrive_and_wait();
    if constexpr (auto _ = timer("reader thread {}", t); std::same_as<FQ, FQSCSP> or std::same_as<FQ, FQUS>) {
        static util::SpinLock read_lock;
        while (true)
            if (std::scoped_lock lock{read_lock}; not fq.consume(consume_func)) break;
    } else if (std::same_as<FQ, FQMCSP>) fq.get_reader(t).template consume_n<false, false>(consume_func, N);
    std::scoped_lock lock{res_vec_mut};
    result_vector.insert(result_vector.end(), res_vec.begin(), res_vec.end());
}

template<typename FQ>
void writer(FQ &fq, size_t seed, size_t t, size_t func_per_thread) {
    static util::SpinLock write_lock;
    size_t functions{0};
    CallbackGenerator callbackGenerator{seed};
    while (functions != func_per_thread and callbackGenerator.addCallback([&](auto &&func) {
        std::scoped_lock lock{write_lock};
        return fq.push(fwd(func));
    }))
        ++functions;
    fmt::print("thread {} wrote {} functions\n", t, functions);
    if (functions != func_per_thread) {
        fmt::print(stderr, "error: could not write {} functions, not enough space.\n", func_per_thread - functions);
        std::exit(EXIT_FAILURE);
    }
}

size_t compute_hash(size_t seed, std::ranges::input_range auto &&range) {
    for (auto r : range) boost::hash_combine(seed, r);
    return seed;
}

template<typename FQ>
size_t test(FQ &fq, size_t buffer_size, size_t numWriterThreads, size_t numReaderThreads, size_t seed) {
    {
        auto _ = timer<"write time">();
        std::vector<std::jthread> writer_threads;
        auto const func_per_thread = buffer_size / (numWriterThreads * 64);
        for (auto t = numWriterThreads; t--;)
            writer_threads.emplace_back(writer<FQ>, std::ref(fq), seed, t, func_per_thread);
    }
    std::vector<size_t> result_vector;
    {
        result_vector.reserve(fq.count());
        std::mutex res_vec_mut;
        std::vector<std::jthread> reader_threads;
        std::latch start_latch{static_cast<std::ptrdiff_t>(numReaderThreads + 1)};
        for (auto t = numReaderThreads; t--;)
            reader_threads.emplace_back(reader<FQ>, std::ref(fq), seed, t, numReaderThreads, std::ref(start_latch),
                                        std::ref(result_vector), std::ref(res_vec_mut));
        start_latch.arrive_and_wait();
    }
    fmt::print("result vector size : {}\n", result_vector.size());
    fmt::print("computing hash ...\n");
    std::ranges::sort(result_vector);
    auto const hash = compute_hash(seed, result_vector);
    fmt::print("result : {}\n\n", hash);
    return hash;
}

int main(int argc, char **argv) {
    if (argc == 1) fmt::print("usage : ./fq_test_mr_mw <buffer_size (MB)> <writer_threads> <reader_threads> <seed>\n");
    auto const args = cmd_line_args(argc, argv);
    constexpr double ONE_MiB = 1024.0 * 1024.0;
    auto const buffer_size = static_cast<size_t>(args(1).and_then(parse<double>).value_or(2.0 * 1024.0) * ONE_MiB);
    auto const numWriterThreads = args(2).and_then(parse<size_t>).value_or(std::thread::hardware_concurrency());
    auto const numReaderThreads = args(3).and_then(parse<size_t>).value_or(std::thread::hardware_concurrency());
    auto const seed = args(4).and_then(parse<size_t>).value_or(std::random_device{}());
    fmt::print("buffer size : {} bytes\n", buffer_size);
    fmt::print("writer threads : {}\n", numWriterThreads);
    fmt::print("reader threads : {}\n", numReaderThreads);
    fmt::print("seed : {}\n\n", seed);
    constexpr size_t size_per_func = 8;
    std::vector<size_t> test_results;
    {
        fmt::print("function queue us ....\n");
        FQUS fq{buffer_size, buffer_size / size_per_func};
        test_results.push_back(test(fq, buffer_size, numWriterThreads, numReaderThreads, seed));
    }
    {
        fmt::print("function queue scsp ....\n");
        FQSCSP fq{buffer_size, buffer_size / size_per_func};
        test_results.push_back(test(fq, buffer_size, numWriterThreads, numReaderThreads, seed));
    }
    {
        fmt::print("function queue mcsp ....\n");
        FQMCSP fq{buffer_size, buffer_size / size_per_func, numReaderThreads};
        test_results.push_back(test(fq, buffer_size, numWriterThreads, numReaderThreads, seed));
    }
    if (not std::ranges::all_of(test_results, std::bind_front(std::ranges::equal_to{}, test_results.front()))) {
        fmt::print("error : test results are not same");
        return EXIT_FAILURE;
    }
}
