#include "ComputeCallbackGenerator.h"
#include "Parse.h"
#include <RingBuffers/BufferQueueMCSP.h>
#include <RingBuffers/FunctionQueueMCSP.h>
#include <RingBuffers/ObjectQueueMCSP.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <latch>
#define BOOST_NO_EXCEPTIONS
#include "timer.hpp"
#include <atomic_queue/atomic_queue.h>
#include <boost/container_hash/hash.hpp>
#include <boost/lockfree/queue.hpp>
#include <fmt/format.h>
#include <folly/MPMCQueue.h>
#include <functional>
#include <mutex>
#include <tbb/concurrent_queue.h>
#include <thread>
#include <vector>

class Obj {
public:
    using URBG = std::mt19937_64;

    Obj() = default;

    explicit Obj(URBG &rng)
        : a{std::invoke(uniform_dist<uint64_t>(&rng))}, b{std::invoke(uniform_dist<float>(&rng))},
          c{std::invoke(uniform_dist<uint32_t>(&rng))} {}

    size_t operator()(Obj::URBG &rng) const {
        auto seed = a;
        rng.seed(seed);
        auto const aa = std::invoke(uniform_dist<uint64_t>(&rng, 0, a));
        auto const bb = std::bit_cast<uint32_t>(std::invoke(uniform_dist(&rng, -b, b)));
        auto const cc = std::invoke(uniform_dist<uint32_t>(&rng, 0, c));
        boost::hash_combine(seed, aa);
        boost::hash_combine(seed, bb);
        boost::hash_combine(seed, cc);
        return seed;
    }

private:
    uint64_t a;
    float b;
    uint32_t c;
};

constexpr bool check_once = false;
constexpr bool release = true;
constexpr size_t N = 5;

using BoostQueue = boost::lockfree::queue<Obj, boost::lockfree::fixed_sized<false>>;
using ObjectQueue = rb::ObjectQueueMCSP<Obj, false>;
using FunctionQueue = rb::FunctionQueueMCSP<size_t(Obj::URBG &), rb::FQOpt::InvokeOnce, false>;
using BufferQueue = rb::BufferQueueMCSP<alignof(Obj), false>;
using TBBQ = tbb::concurrent_queue<Obj>;
using FollyQueue = folly::MPMCQueue<Obj>;
using AtomicQueue = atomic_queue::AtomicQueueB2<Obj>;

size_t calculateAndDisplayFinalHash(std::span<size_t> final_result) {
    fmt::print("result vector size : {}\n", final_result.size());
    std::ranges::sort(final_result);
    auto const hash_result = boost::hash_range(final_result.begin(), final_result.end());
    fmt::print("result hash : {}\n", hash_result);
    return hash_result;
}

template<typename T, typename... C>
concept same_as_one_of = (std::same_as<T, C> or ...);

template<typename Obj>
    requires std::is_object_v<Obj>
auto make_object(Obj &obj) {
    return [&](std::span<std::byte> buffer) {
        std::construct_at(reinterpret_cast<Obj *>(buffer.data()), mov(obj));
        return buffer.first(sizeof(Obj));
    };
}

template<typename Obj>
    requires std::is_object_v<Obj>
auto consume_object(std::invocable<Obj &> auto &&func) {
    return [&](std::span<std::byte> buffer) { std::invoke(func, *reinterpret_cast<Obj *>(buffer.data())); };
}

void wait() { std::this_thread::sleep_for(std::chrono::nanoseconds{1}); }

template<size_t N>
bool copy_consume_n(auto &reader, auto &&func) {
    std::array<Obj, N> storage;
    auto const n = reader.template consume_n<check_once, release>(
            [&, i = size_t{}](Obj const &obj) mutable { storage[i++] = obj; }, N);
    std::ranges::for_each(std::span{storage.data(), n}, func);
    return n;
}

template<same_as_one_of<ObjectQueue, FunctionQueue, BufferQueue, TBBQ, BoostQueue, AtomicQueue, FollyQueue> OQ>
bool empty(OQ &oq) {
    if constexpr (std::same_as<OQ, FollyQueue>) return oq.isEmpty();
    else if constexpr (std::same_as<OQ, AtomicQueue>) return oq.was_empty();
    else return oq.empty();
}

template<same_as_one_of<ObjectQueue, FunctionQueue, BufferQueue, TBBQ, BoostQueue, FollyQueue, AtomicQueue> OQ>
size_t test(OQ &oq, size_t threads, size_t objects, size_t seed) {
    std::vector<uint64_t> final_result;
    {
        std::atomic<bool> is_done{false};
        std::latch start_latch{static_cast<ssize_t>(threads + 1)};
        std::jthread writer{[&start_latch, &oq, &is_done, objects, seed] {
            auto rng = Obj::URBG{seed};
            start_latch.arrive_and_wait();
            for (auto o = objects; o--;)
                if constexpr (same_as_one_of<OQ, TBBQ, AtomicQueue>) oq.push(Obj{rng});
                else if constexpr (same_as_one_of<OQ, ObjectQueue, FunctionQueue, BoostQueue>)
                    for (Obj obj{rng}; not oq.push(obj); wait());
                else if constexpr (std::same_as<OQ, FollyQueue>)
                    for (Obj obj{rng}; not oq.write(obj); wait());
                else if constexpr (std::same_as<OQ, BufferQueue>)
                    for (Obj obj{rng}; not oq.allocate_and_release(sizeof(Obj), alignof(Obj), make_object(obj));
                         wait());
            is_done.store(true, std::memory_order::release);
            fmt::print("writer thread finished, objects processed : {}\n", objects);
        }};
        std::vector<std::jthread> reader_threads;
        std::mutex final_result_mutex;
        for (size_t thread_id{0}; thread_id != threads; ++thread_id)
            reader_threads.emplace_back([&start_latch, &oq, &is_done, &final_result_mutex, &final_result, seed,
                                         thread_id, object_per_thread = objects / threads] {
                auto rng = Obj::URBG{seed};
                std::vector<uint64_t> local_result;
                local_result.reserve(object_per_thread);
                start_latch.arrive_and_wait();
                for (auto _ = timer("thread {}", thread_id);
                     not(is_done.load(std::memory_order::acquire) and empty(oq)); wait())
                    if constexpr (std::same_as<OQ, BoostQueue>)
                        for (Obj obj; oq.pop(obj);) local_result.push_back(obj(rng));
                    else if constexpr (std::same_as<OQ, FollyQueue>)
                        for (Obj obj; oq.read(obj);) local_result.push_back(obj(rng));
                    else if constexpr (std::same_as<OQ, AtomicQueue>)
                        for (Obj obj; oq.try_pop(obj);) local_result.push_back(obj(rng));
                    else if constexpr (std::same_as<OQ, ObjectQueue>)
                        for (auto reader = oq.get_reader(thread_id);
                             copy_consume_n<N>(reader, [&](auto &obj) { local_result.push_back(obj(rng)); }););
                    else if constexpr (std::same_as<OQ, FunctionQueue>)
                        for (auto reader = oq.get_reader(thread_id); reader.template consume_n<check_once, release>(
                                     [&](auto func) { local_result.push_back(func(rng)); }, N););
                    else if constexpr (std::same_as<OQ, BufferQueue>)
                        for (auto reader = oq.get_reader(thread_id); reader.template consume_n<check_once, release>(
                                     consume_object<Obj>([&](auto &obj) { local_result.push_back(obj(rng)); }), N););
                    else if constexpr (std::same_as<OQ, TBBQ>)
                        for (Obj obj; oq.try_pop(obj);) local_result.push_back(obj(rng));
                std::scoped_lock lock{final_result_mutex};
                final_result.insert(final_result.end(), local_result.begin(), local_result.end());
            });
    }
    return calculateAndDisplayFinalHash(final_result);
}

int main(int argc, char **argv) {
    if (argc == 1) fmt::print("usage : ./oq_test_nr_1w <objects> <reader-threads> <seed> <capacity>\n");
    auto const args = cmd_line_args(argc, argv);
    auto const objects = args(1).and_then(parse<size_t>).value_or(10'000'000);
    auto const reader_threads = args(2).and_then(parse<size_t>).value_or(std::thread::hardware_concurrency());
    auto const seed = args(3).and_then(parse<size_t>).value_or(std::random_device{}());
    auto const capacity = args(4).and_then(parse<size_t>).value_or(100'000);
    fmt::print("objects to process : {}\n", objects);
    fmt::print("reader threads : {}\n", reader_threads);
    fmt::print("seed : {}\n", seed);
    fmt::print("capacity : {}\n", capacity);
    std::vector<size_t> test_results;
    {
        fmt::print("\nBoost Queue ....\n");
        BoostQueue boostQueue{capacity};
        test_results.push_back(test(boostQueue, reader_threads, objects, seed));
    }
    {
        fmt::print("\nTBB Queue ....\n");
        TBBQ tbbQueue{};
        test_results.push_back(test(tbbQueue, reader_threads, objects, seed));
    }
    {
        fmt::print("\nFolly Queue ....\n");
        FollyQueue follyQueue{capacity};
        test_results.push_back(test(follyQueue, reader_threads, objects, seed));
    }
    {
        fmt::print("\nAtomic Queue ....\n");
        AtomicQueue atomicQueue{static_cast<uint32_t>(capacity)};
        test_results.push_back(test(atomicQueue, reader_threads, objects, seed));
    }
    {
        fmt::print("\nObject Queue ....\n");
        ObjectQueue objectQueue{capacity, reader_threads};
        test_results.push_back(test(objectQueue, reader_threads, objects, seed));
    }
    {
        fmt::print("\nFunction Queue ....\n");
        FunctionQueue functionQueue{capacity * sizeof(Obj), capacity, reader_threads};
        test_results.push_back(test(functionQueue, reader_threads, objects, seed));
    }
    {
        fmt::print("\nBuffer Queue ....\n");
        BufferQueue bufferQueue{sizeof(Obj) * capacity, capacity, reader_threads};
        test_results.push_back(test(bufferQueue, reader_threads, objects, seed));
    }
    if (std::ranges::adjacent_find(test_results, std::not_equal_to{}) != test_results.end()) {
        fmt::print("error : test results are not same");
        return EXIT_FAILURE;
    }
}

void boost::throw_exception(std::exception const &e, boost::source_location const &l) {
    fmt::print(stderr, "{} {}\n", l.to_string(), e.what());
    std::terminate();
}
void boost::throw_exception(std::exception const &e) {
    fmt::print(stderr, "{}\n", e.what());
    std::terminate();
}
