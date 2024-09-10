#ifndef FIXED_STRING_H
#define FIXED_STRING_H

#include <algorithm>
#include <array>
#include <string_view>

template<typename char_t, size_t N>
struct fixed_string {
    using value_type = char_t;
    using str_view = std::basic_string_view<value_type>;

    constexpr fixed_string(str_view str) {
        if constexpr (N) std::ranges::copy_n(str.data(), N, m_Value.data());
    }

    constexpr fixed_string(value_type const (&str)[N + 1]) : fixed_string{str_view{str, N}} {}

    constexpr str_view value() const {
        if constexpr (N) return str_view{m_Value};
        else return {};
    }

    constexpr bool operator==(str_view str) const { return value() == str; }

    constexpr operator str_view() const { return value(); }

    std::array<value_type, N> m_Value;
};

template<typename char_t, size_t N>
fixed_string(const char_t (&str)[N]) -> fixed_string<char_t, N - 1>;

template<fixed_string str, size_t pos, size_t count = std::string_view::npos>
    requires(pos < str.value().size())
inline constexpr auto substr = [] {
    constexpr auto strv = str.value();
    constexpr size_t rcount = std::min(count, strv.size() - pos);
    return fixed_string<typename decltype(str)::value_type, rcount>{strv.substr(pos, rcount)};
}();

template<fixed_string str>
consteval auto operator""_fs() {
    return str;
}

#endif
