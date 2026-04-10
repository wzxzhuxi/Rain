#pragma once

#include "types.hpp"

#include <expected>
#include <format>
#include <source_location>
#include <system_error>

namespace rain {

struct SystemError {
    std::error_code code;
    String message;
    std::source_location location;

    [[nodiscard]] static auto from_errno(std::source_location loc = std::source_location::current()) -> SystemError
    {
        const int err = errno;
        return { .code = std::error_code(err, std::system_category()),
                 .message = std::system_category().message(err),
                 .location = loc };
    }

    [[nodiscard]] auto format() const -> String
    {
        return std::format("{}:{} [{}] {}", location.file_name(), location.line(), code.value(), message);
    }

    [[nodiscard]] auto to_string() const -> String { return format(); }
};

template<typename T, typename E = SystemError>
using Result = std::expected<T, E>;

template<typename T>
[[nodiscard]] constexpr auto Ok(T&& value) -> Result<std::decay_t<T>>
{
    return Result<std::decay_t<T>>(std::forward<T>(value));
}

[[nodiscard]] constexpr auto Ok() -> Result<Unit> {
    return Result<Unit>(unit);
}

template<typename E>
[[nodiscard]] constexpr auto Err(E&& error) -> std::unexpected<std::decay_t<E>>
{
    return std::unexpected(std::forward<E>(error));
}

} // namespace rain
