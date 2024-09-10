#include "ComputeCallbackGenerator.h"
#include "Parse.h"
#include <RingBuffers/BufferQueueMCSP.h>
#include <RingBuffers/BufferQueueSCSP.h>
#include <RingBuffers/FunctionQueueMCSP.h>
#include <RingBuffers/FunctionQueueSCSP.h>
#include <RingBuffers/ObjectQueueMCSP.h>
#include <RingBuffers/ObjectQueueSCSP.h>
#include <atomic_queue/atomic_queue.h>
#include <exception>
#include <folly/ProducerConsumerQueue.h>
#include <latch>
#include <thread>
#define BOOST_NO_EXCEPTIONS
#include "timer.hpp"
#include <boost/circular_buffer.hpp>
#include <boost/container_hash/hash.hpp>
#include <boost/lockfree/policies.hpp>
#include <boost/lockfree/queue.hpp>
#include <boost/lockfree/spsc_queue.hpp>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <tbb/concurrent_queue.h>

class Obj {
public:
    using URBG = std::mt19937_64;

    Obj() = default;

    explicit Obj(URBG &rng)
        : a{std::invoke(uniform_dist<uint64_t>(&rng))}, b{std::invoke(uniform_dist<float>(&rng))},
          c{std::invoke(uniform_dist<uint32_t>(&rng))} {}

    size_t operator()(Obj::URBG &rng, size_t seed) const {
        rng.seed(seed);
        auto const aa = std::invoke(uniform_dist<uint64_t>(&rng, 0, a));
        auto const bb = std::bit_cast<uint32_t>(std::invoke(uniform_dist(&rng, -b, b)));
        auto const cc = std::invoke(uniform_dist<uint32_t>(&rng, 0, c));
        boost::hash_combine(seed, aa);
        boost::hash_combine(seed, bb);
        boost::hash_combine(seed, cc);
        boost::hash_combine(seed, 21298214897uz * aa);
        boost::hash_combine(seed, 982138.124214 * cc);
        boost::hash_combine(seed, -12907892);
        boost::hash_combine(seed, -918289241948z);
        return seed;
    }

private:
    uint64_t a;
    float b;
    uint32_t c;
};

constexpr bool check_once = true;

using BoostQueueSCSP = boost::lockfree::spsc_queue<Obj, boost::lockfree::fixed_sized<false>>;
using BoostQueueMCMP = boost::lockfree::queue<Obj, boost::lockfree::fixed_sized<true>>;
using OQSCSP = rb::ObjectQueueSCSP<Obj, true>;
using OQMCSP = rb::ObjectQueueMCSP<Obj, true>;
using FQSCSP = rb::FunctionQueueSCSP<size_t(Obj::URBG &, size_t), rb::FQOpt::InvokeOnce, true>;
using FQMCSP = rb::FunctionQueueMCSP<size_t(Obj::URBG &, size_t), rb::FQOpt::InvokeOnce, true>;
using BQSCSP = rb::BufferQueueSCSP<alignof(Obj), true>;
using BQMCSP = rb::BufferQueueMCSP<alignof(Obj), true>;
using TBBQ = tbb::concurrent_queue<Obj>;
using FollyQueue = folly::ProducerConsumerQueue<Obj>;
using AtomicQueue = atomic_queue::AtomicQueueB2<Obj, std::allocator<Obj>, true, false, true>;

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

void wait() { std::this_thread::sleep_for(std::chrono::nanoseconds{1}); }

template<same_as_one_of<AtomicQueue, BoostQueueSCSP, BoostQueueMCMP, FollyQueue, OQSCSP, OQMCSP, BQSCSP, BQMCSP, FQSCSP,
                        FQMCSP, TBBQ>
                 OQ>
size_t test(OQ &oq, size_t objects, size_t seed) {
    std::latch start_latch{2};
    std::jthread writer{[&oq, &start_latch, objects, seed] {
        auto rng = Obj::URBG{seed};
        start_latch.arrive_and_wait();
        for (auto o = objects; o--;)
            if constexpr (same_as_one_of<OQ, TBBQ, AtomicQueue>) oq.push(Obj{rng});
            else if constexpr (same_as_one_of<OQ, OQSCSP, OQMCSP, FQSCSP, FQMCSP, BoostQueueSCSP, BoostQueueMCMP>)
                for (Obj obj{rng}; not oq.push(obj); wait());
            else if constexpr (std::same_as<OQ, FollyQueue>)
                for (Obj obj{rng}; not oq.write(obj); wait());
            else if constexpr (same_as_one_of<OQ, BQSCSP, BQMCSP>)
                for (Obj obj{rng}; not oq.allocate_and_release(sizeof(Obj), alignof(Obj), make_object(obj)); wait());
    }};
    std::jthread reader{[&oq, &start_latch, &seed, objects] {
        auto rng = Obj::URBG{seed};
        start_latch.arrive_and_wait();
        auto _ = timer<"read time">();
        auto consume_func = [&](Obj const &obj) { seed = obj(rng, seed); };
        auto fconsume_func = [&](auto func) { seed = func(rng, seed); };
        auto bconsume_func = [&](std::span<std::byte> b) { consume_func(*reinterpret_cast<Obj *>(b.data())); };
        for (auto obj = objects; obj;)
            if constexpr (std::same_as<OQ, AtomicQueue>) consume_func(oq.pop()), --obj;
            else if constexpr (std::same_as<OQ, FollyQueue>) {
                if (Obj o; oq.read(o)) consume_func(o), --obj;
            } else if constexpr (std::same_as<OQ, OQSCSP>) obj -= (oq.wait(), oq.consume_all(consume_func));
            else if constexpr (std::same_as<OQ, OQMCSP>)
                obj -= (oq.wait(), oq.get_reader(0).template consume_all<check_once>(consume_func));
            else if constexpr (std::same_as<OQ, FQSCSP>) obj -= (oq.wait(), oq.consume_all(fconsume_func));
            else if constexpr (std::same_as<OQ, FQMCSP>)
                obj -= (oq.wait(), oq.get_reader(0).template consume_all<check_once>(fconsume_func));
            else if constexpr (std::same_as<OQ, BQSCSP>) obj -= (oq.wait(), oq.consume_all(bconsume_func));
            else if constexpr (std::same_as<OQ, BQMCSP>)
                obj -= (oq.wait(), oq.get_reader(0).template consume_all<check_once>(bconsume_func));
            else if constexpr (std::same_as<OQ, TBBQ>) {
                if (Obj o; oq.try_pop(o)) consume_func(o), --obj;
            } else if constexpr (same_as_one_of<OQ, BoostQueueSCSP, BoostQueueMCMP>) {
                obj -= oq.consume_all(consume_func);
            }
    }};
    writer.join();
    reader.join();
    fmt::print("hash of {} objects : {}\n", objects, seed);
    return seed;
}

int main(int argc, char **argv) {
    if (argc == 1) fmt::print("usage : ./oq_test_1r_1w <objects> <seed>\n");
    auto const args = cmd_line_args(argc, argv);
    auto const objects = args(1).and_then(parse<size_t>).value_or(2'000'000);
    auto const seed = args(2).and_then(parse<size_t>).value_or(std::random_device{}());
    fmt::print("objects : {}\n", objects);
    fmt::print("seed : {}\n", seed);
    constexpr size_t capacity = 65534;
    std::vector<size_t> test_results;
    {
        fmt::print("\nboost queue scsp ...\n");
        BoostQueueSCSP boostQueue{capacity};
        test_results.push_back(test(boostQueue, objects, seed));
    }
    {
        fmt::print("\nboost queue mcmp ...\n");
        BoostQueueMCMP boostQueue{capacity};
        test_results.push_back(test(boostQueue, objects, seed));
    }
    {
        fmt::print("\nTBB queue ...\n");
        TBBQ tbbQueue{};
        test_results.push_back(test(tbbQueue, objects, seed));
    }
    {
        fmt::print("\nAtomic queue ...\n");
        AtomicQueue atomicQueue{capacity};
        test_results.push_back(test(atomicQueue, objects, seed));
    }
    {
        fmt::print("\nFolly queue ...\n");
        FollyQueue follyQueue{capacity};
        test_results.push_back(test(follyQueue, objects, seed));
    }
    {
        fmt::print("\nobject queue scsp ...\n");
        OQSCSP objectQueue{capacity};
        test_results.push_back(test(objectQueue, objects, seed));
    }
    {
        fmt::print("\nobject queue mcsp ...\n");
        OQMCSP objectQueue{capacity, 1};
        test_results.push_back(test(objectQueue, objects, seed));
    }
    {
        fmt::print("\nbuffer queue scsp ...\n");
        BQSCSP bufferQueue{sizeof(Obj) * capacity, capacity};
        test_results.push_back(test(bufferQueue, objects, seed));
    }
    {
        fmt::print("\nbuffer queue mcsp ...\n");
        BQMCSP bufferQueue{sizeof(Obj) * capacity, capacity, 1};
        test_results.push_back(test(bufferQueue, objects, seed));
    }
    {
        fmt::print("\nfunction queue scsp ...\n");
        FQSCSP funtionQueue{sizeof(Obj) * capacity, capacity};
        test_results.push_back(test(funtionQueue, objects, seed));
    }
    {
        fmt::print("\nfunction queue mcsp ...\n");
        FQMCSP funtionQueue{sizeof(Obj) * capacity, capacity, 1};
        test_results.push_back(test(funtionQueue, objects, seed));
    }
    if (not std::ranges::all_of(test_results, std::bind_front(std::ranges::equal_to{}, test_results.front()))) {
        fmt::print("error : test results are not same");
        return EXIT_FAILURE;
    }
}

void boost::throw_exception(std::exception const &e) {
    fmt::print(stderr, "{}\n", e.what());
    std::terminate();
}
