#ifndef FUNCTION_WRAPPER
#define FUNCTION_WRAPPER

#include "detail/move_forward.hpp"
#include <functional>
#include <type_traits>

namespace rb {
template<auto func>
    requires(std::is_function_v<std::remove_pointer_t<decltype(func)>>)
            inline constexpr auto function = []<typename... Args>
                requires std::invocable<decltype(func), Args...>
(Args &&...args) -> decltype(auto) { return std::invoke(func, fwd(args)...); };
}// namespace rb

#endif
