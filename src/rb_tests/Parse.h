#ifndef PARSE
#define PARSE

#include <charconv>
#include <optional>
#include <string_view>

template<typename value_type>
    requires std::is_arithmetic_v<value_type>
std::optional<value_type> parse(std::string_view str) {
    if (value_type value; std::from_chars(str.data(), str.data() + str.size(), value).ec == std::errc{}) return value;
    return {};
}

inline auto cmd_line_args(int argc, char **argv) {
    return [=](int i) -> std::optional<std::string_view> {
        if (i < argc) return argv[i];
        return {};
    };
}

#endif
