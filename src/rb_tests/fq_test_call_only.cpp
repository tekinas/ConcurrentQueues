#include "ComputeCallbackGenerator.h"
#include "Parse.h"
#include "timer.hpp"
#include <RingBuffers/FunctionQueue.h>
#include <RingBuffers/FunctionQueueMCSP.h>
#include <RingBuffers/FunctionQueueSCSP.h>
#include <fmt/format.h>
#include <folly/Function.h>
#include <ranges>

using ComputeFunctionSig = size_t(size_t);
using FQUS = rb::FunctionQueue<ComputeFunctionSig, rb::FQOpt::InvokeOnceDNI>;
using FQSCSP = rb::FunctionQueueSCSP<ComputeFunctionSig, rb::FQOpt::InvokeOnce, false>;
using FQMCSP = rb::FunctionQueueMCSP<ComputeFunctionSig, rb::FQOpt::InvokeMultiple, true>;

template<typename FQ>
void test(FQ &fq) {
    size_t num{};
    if constexpr (std::same_as<FQ, FQMCSP>)
        timer<"function queue mcsp">(),
                fq.get_reader(0).template consume_all<true>([&](auto func) { num = func(num); });
    else if constexpr (std::same_as<FQ, FQSCSP>)
        timer<"function queue scsp">(), fq.consume_all([&](auto func) { num = func(num); });
    else if constexpr (std::same_as<FQ, FQUS>)
        timer<"function queue us">(), fq.consume_all([&](auto func) { num = func(num); });
    fmt::print("result : {}\n\n", num);
}

void test(std::vector<folly::Function<ComputeFunctionSig>> &fq) {
    size_t num{};
    for (auto _ = timer<"std::vector<folly::Function>">(); auto &func : fq) num = func(num);
    fmt::print("result : {}\n\n", num);
}

void test(std::vector<std::move_only_function<ComputeFunctionSig>> &fq) {
    size_t num{};
    for (auto _ = timer<"std::vector<std::move_only_function>">(); auto &func : fq) num = func(num);
    fmt::print("result : {}\n\n", num);
}

int main(int argc, char **argv) {
    if (argc == 1) fmt::print("usage : ./fq_test_call_only <buffer_size (MB)> <functions> <seed>\n");
    auto const args = cmd_line_args(argc, argv);
    constexpr double ONE_MiB = 1024.0 * 1024.0;
    auto const buffer_size = static_cast<size_t>(args(1).and_then(parse<double>).value_or(500.0) * ONE_MiB);
    auto const functions = args(2).and_then(parse<size_t>).value_or(10'000'000);
    auto const seed = args(3).and_then(parse<size_t>).value_or(std::random_device{}());
    fmt::print("buffer size : {} bytes\n", buffer_size);
    fmt::print("functions : {}\n", functions);
    fmt::print("seed : {}\n", seed);
    CallbackGenerator cbg{seed};
    FQUS fqus{buffer_size, functions};
    auto const func_emplaced = [&] {
        auto _ = timer<"function queue us write time">();
        for (size_t functions{0};;)
            if (cbg.addCallback([&](auto &&func) { return fqus.push(fwd(func)); })) ++functions;
            else return functions;
    }();
    auto fill = [&](std::string tn, auto sink) {
        cbg.setSeed(seed);
        auto _ = timer(tn);
        for (auto _ : std::views::iota(0u, func_emplaced)) cbg.addCallback(sink);
    };
    FQSCSP fqscsp{buffer_size, functions};
    FQMCSP fqmcsp{buffer_size, functions, 1};
    std::vector<folly::Function<ComputeFunctionSig>> follyFunctionVector{};
    std::vector<std::move_only_function<ComputeFunctionSig>> stdFuncionVector{};
    follyFunctionVector.reserve(func_emplaced);
    stdFuncionVector.reserve(func_emplaced);
    fill("std::vector<folly::Functions> write time", [&](auto &&func) { follyFunctionVector.emplace_back(func); });
    fill("std::vector<std::move_only_functions> write time", [&](auto &&func) { stdFuncionVector.emplace_back(func); });
    fill("function queue scsp write time", [&](auto &&func) { fqscsp.push(func); });
    fill("function queue mcsp write time", [&](auto &&func) { fqmcsp.push(func); });
    fmt::print("\nfunctions emplaced : {}\n\n", func_emplaced);
    test(follyFunctionVector);
    test(stdFuncionVector);
    test(fqus);
    test(fqscsp);
    test(fqmcsp);
}
