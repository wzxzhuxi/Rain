#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/task.hpp"
#include "fs/detail.hpp"
#include "fs/metadata.hpp"

#include <cstring>
#include <utility>

namespace rain::fs {

[[nodiscard]] inline auto read(String path) -> async::Task<Result<Bytes>>
{
    co_return co_await detail::spawn_blocking_result([path = std::move(path)] {
        return detail::read_file_sync(path);
    });
}

[[nodiscard]] inline auto read_to_string(String path) -> async::Task<Result<String>>
{
    auto bytes = co_await read(std::move(path));
    if (!bytes) {
        co_return Err(std::move(bytes).error());
    }

    String text;
    text.resize(bytes->size());
    if (!bytes->empty()) {
        std::memcpy(text.data(), bytes->data(), bytes->size());
    }
    co_return Ok(std::move(text));
}

[[nodiscard]] inline auto write(String path, Bytes data) -> async::Task<Result<Unit>>
{
    co_return co_await detail::spawn_blocking_result([path = std::move(path), data = std::move(data)] {
        return detail::write_file_sync(path, data.data(), data.size(), false);
    });
}

[[nodiscard]] inline auto write_string(String path, String data) -> async::Task<Result<Unit>>
{
    co_return co_await detail::spawn_blocking_result([path = std::move(path), data = std::move(data)] {
        return detail::write_file_sync(path, reinterpret_cast<const u8*>(data.data()), data.size(), false);
    });
}

[[nodiscard]] inline auto append(String path, Bytes data) -> async::Task<Result<Unit>>
{
    co_return co_await detail::spawn_blocking_result([path = std::move(path), data = std::move(data)] {
        return detail::write_file_sync(path, data.data(), data.size(), true);
    });
}

[[nodiscard]] inline auto append_string(String path, String data) -> async::Task<Result<Unit>>
{
    co_return co_await detail::spawn_blocking_result([path = std::move(path), data = std::move(data)] {
        return detail::write_file_sync(path, reinterpret_cast<const u8*>(data.data()), data.size(), true);
    });
}

[[nodiscard]] inline auto exists(String path) -> async::Task<Result<bool>>
{
    co_return co_await detail::spawn_blocking_result([path = std::move(path)] {
        return detail::exists_sync(path);
    });
}

[[nodiscard]] inline auto metadata(String path) -> async::Task<Result<Metadata>>
{
    co_return co_await detail::spawn_blocking_result([path = std::move(path)] {
        return detail::metadata_path_sync(path, true);
    });
}

[[nodiscard]] inline auto symlink_metadata(String path) -> async::Task<Result<Metadata>>
{
    co_return co_await detail::spawn_blocking_result([path = std::move(path)] {
        return detail::metadata_path_sync(path, false);
    });
}

[[nodiscard]] inline auto remove_file(String path) -> async::Task<Result<Unit>>
{
    co_return co_await detail::spawn_blocking_result([path = std::move(path)] {
        return detail::remove_file_sync(path);
    });
}

} // namespace rain::fs
