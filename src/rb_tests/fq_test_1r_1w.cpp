#include "ComputeCallbackGenerator.h"
#include "Parse.h"
#include "SpinLock.h"
#include "timer.hpp"
#include <RingBuffers/FunctionQueue.h>
#include <RingBuffers/FunctionQueueMCSP.h>
#include <RingBuffers/FunctionQueueSCSP.h>
#include <fmt/format.h>
#include <latch>
#include <limits>
#include <mutex>
#include <random>
#include <thread>

using namespace rb;
using ComputeFunctionSig = size_t(size_t);
using FQUS = rb::FunctionQueue<ComputeFunctionSig, FQOpt::InvokeOnceDNI>;
using FQSCSP = rb::FunctionQueueSCSP<ComputeFunctionSig, FQOpt::InvokeOnceDNI, false>;
using FQMCSP = rb::FunctionQueueMCSP<ComputeFunctionSig, FQOpt::InvokeOnce, false>;

static constexpr auto sentinel = std::numeric_limits<size_t>::max();
constexpr auto sentinel_func = [](auto) { return sentinel; };

template<typename FQ>
    requires(std::same_as<FQ, FQSCSP> or std::same_as<FQ, FQMCSP>)
void test(FQ &fq, size_t seed, size_t functions) {
    std::latch start_latch{2};
    std::jthread writer{[&] {
        CallbackGenerator callbackGenerator{seed};
        start_latch.arrive_and_wait();
        for (auto f = functions; f--;) callbackGenerator.addCallback([&](auto &&func) { while (not fq.push(func)); });
        while (not fq.push(sentinel_func));
    }};
    std::jthread reader{[&] {
        start_latch.arrive_and_wait();
        size_t num{0};
        ScopeGaurd pr = [&] { fmt::print("result : {}\n", num); };
        auto _ = timer<"reader">();
        for (bool quit = false; not quit;) {
            rb::wait<5>([&] { return not fq.empty(); });
            auto consume_func = [&quit, &num](auto func) {
                if (auto const res = func(num); res != sentinel) num = res;
                else quit = true;
            };
            if constexpr (std::same_as<FQ, FQSCSP>) fq.consume_all(consume_func);
            else fq.get_reader(0).template consume_all<true>(consume_func);
        }
    }};
}

void test(FQUS &fq, size_t seed, size_t functions) {
    std::latch start_latch{2};
    auto op = [qlock = util::SpinLock{}](auto &&func) mutable {
        std::scoped_lock _{qlock};
        return func();
    };
    std::jthread writer{[&] {
        CallbackGenerator callbackGenerator{seed};
        start_latch.arrive_and_wait();
        for (auto f = functions; f--;)
            callbackGenerator.addCallback([&](auto &&func) { while (not op([&] { return fq.push(func); })); });
        while (not op([&] { return fq.push(sentinel_func); }));
    }};
    std::jthread reader{[&] {
        start_latch.arrive_and_wait();
        size_t num{0};
        ScopeGaurd pr = [&] { fmt::print("result : {}\n", num); };
        auto _ = timer<"reader">();
        for (bool quit = false; not quit;)
            op([&] {
                fq.consume([&](auto func) {
                    if (auto const res = func(num); res != sentinel) num = res;
                    else quit = true;
                });
            });
    }};
}

int main(int argc, char **argv) {
    if (argc == 1) fmt::print("usage : ./fq_test_1r_1w <buffer_size> <functions> <seed>\n");
    auto const args = cmd_line_args(argc, argv);
    constexpr double ONE_MiB = 1024.0 * 1024.0;
    auto const buffer_size = static_cast<size_t>(args(1).and_then(parse<double>)
                                                         .transform(std::bind_front(std::multiplies{}, ONE_MiB))
                                                         .value_or(0.1 * ONE_MiB));
    auto const functions = args(2).and_then(parse<size_t>).value_or(20'000'000);
    auto const seed = args(3).and_then(parse<size_t>).value_or(std::random_device{}());
    fmt::print("buffer size : {}\n", buffer_size);
    fmt::print("functions : {}\n", functions);
    fmt::print("seed : {}\n", seed);
    {
        fmt::print("\nfunction queue unsynced ...\n");
        FQUS fq{buffer_size, 10'000};
        test(fq, seed, functions);
    }
    {
        fmt::print("\nfunction queue scsp ...\n");
        FQSCSP fq{buffer_size, 10'000};
        test(fq, seed, functions);
    }
    {
        fmt::print("\nfunction queue mcsp ...\n");
        FQMCSP fq{buffer_size, 10'000, 1};
        test(fq, seed, functions);
    }
}
