#ifndef TIMER_H
#define TIMER_H

#include "fixed_string.hpp"
#include "scope.hpp"
#include <chrono>
#include <fmt/chrono.h>
#include <fmt/compile.h>
#include <fmt/format.h>

template<fixed_string name, typename Period = std::ratio<1>, typename Duration = std::chrono::duration<double, Period>>
auto timer() {
    using Clock = std::chrono::steady_clock;
    return ScopeGaurd{[start_tp = Clock::now()] {
        auto const time_elapsed = std::chrono::duration_cast<Duration>(Clock::now() - start_tp);
        fmt::print(FMT_COMPILE("{} : {}\n"), name.value(), time_elapsed);
    }};
}

template<typename Period = std::ratio<1>, typename Duration = std::chrono::duration<double, Period>, typename... Args>
    requires(sizeof...(Args) != 0)
auto timer(fmt::format_string<Args...> fmt_str, Args &&...args) {
    using Clock = std::chrono::steady_clock;
    return ScopeGaurd{[name = fmt::format(fmt_str, fwd(args)...), start_tp = Clock::now()] {
        auto const time_elapsed = std::chrono::duration_cast<Duration>(Clock::now() - start_tp);
        fmt::print(FMT_COMPILE("{} : {}\n"), name, time_elapsed);
    }};
}

template<typename Period = std::ratio<1>, typename Duration = std::chrono::duration<double, Period>>
auto timer(std::string_view name) {
    using Clock = std::chrono::steady_clock;
    return ScopeGaurd{[name, start_tp = Clock::now()] {
        auto const time_elapsed = std::chrono::duration_cast<Duration>(Clock::now() - start_tp);
        fmt::print(FMT_COMPILE("{} : {}\n"), name, time_elapsed);
    }};
}

template<typename Period = std::ratio<1>, typename Duration = std::chrono::duration<double, Period>>
auto timer(std::string &&name) {
    using Clock = std::chrono::steady_clock;
    return ScopeGaurd{[name = mov(name), start_tp = Clock::now()] {
        auto const time_elapsed = std::chrono::duration_cast<Duration>(Clock::now() - start_tp);
        fmt::print(FMT_COMPILE("{} : {}\n"), name, time_elapsed);
    }};
}

#endif
