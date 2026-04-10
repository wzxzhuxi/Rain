#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace rain {

using Byte = std::byte;

using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using usize = std::size_t;
using isize = std::ptrdiff_t;

using f32 = float;
using f64 = double;

using String = std::string;
using StringView = std::string_view;

using ByteSpan = std::span<std::byte>;
using ConstByteSpan = std::span<const std::byte>;
using CharSpan = std::span<char>;
using ConstCharSpan = std::span<const char>;

template<typename T>
using Box = std::unique_ptr<T>;

template<typename T>
using Weak = std::weak_ptr<T>;

template<typename T>
using Shared = std::shared_ptr<T>;

template<typename T>
using Ref = std::reference_wrapper<T>;

struct Unit {
  constexpr auto operator==(const Unit&) const -> bool = default;
};
inline constexpr Unit unit{};

} // namespace rain
