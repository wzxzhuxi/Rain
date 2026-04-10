#pragma once

#include "types.hpp"

#include <concepts>
#include <functional>
#include <ranges>

namespace rain {

template<typename T>
concept Hashable = requires(T a) {
    { std::hash<T> {}(a) } -> std::convertible_to<usize>;
};

template<typename T>
concept Comparable = std::totally_ordered<T>;

template<typename T>
concept Stringable = requires(T a) {
    { a.to_string() } -> std::convertible_to<String>;
};

template<typename T>
concept ByteRange = std::ranges::range<T>
                    && (std::same_as<std::ranges::range_value_t<T>, char>
                        || std::same_as<std::ranges::range_value_t<T>, unsigned char>
                        || std::same_as<std::ranges::range_value_t<T>, Byte>);

} // namespace rain
