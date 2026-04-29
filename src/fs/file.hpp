#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include "async/task.hpp"
#include "fs/detail.hpp"
#include "fs/metadata.hpp"

#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace rain::fs {

struct OpenOptions {
    bool read = true;
    bool write = false;
    bool append = false;
    bool truncate = false;
    bool create = false;
    bool create_new = false;
    mode_t mode = 0644;

    [[nodiscard]] static auto read_only() -> OpenOptions { return { .read = true }; }
    [[nodiscard]] static auto write_only() -> OpenOptions { return { .read = false, .write = true }; }
    [[nodiscard]] static auto read_write() -> OpenOptions { return { .read = true, .write = true }; }
    [[nodiscard]] static auto create_truncate() -> OpenOptions
    {
        return { .read = false, .write = true, .truncate = true, .create = true };
    }
    [[nodiscard]] static auto append_create() -> OpenOptions
    {
        return { .read = false, .write = true, .append = true, .create = true };
    }
};

namespace detail {

[[nodiscard]] inline auto open_flags(const OpenOptions& options) -> Result<i32>
{
    i32 flags = 0;
    const bool needs_write = options.write || options.append || options.truncate || options.create || options.create_new;

    if (options.read && needs_write) {
        flags |= O_RDWR;
    } else if (options.read) {
        flags |= O_RDONLY;
    } else if (needs_write) {
        flags |= O_WRONLY;
    } else {
        return Err(make_error(std::errc::invalid_argument, "OpenOptions must enable read or write access"));
    }

    if (options.append) {
        flags |= O_APPEND;
    }
    if (options.truncate) {
        flags |= O_TRUNC;
    }
    if (options.create || options.create_new) {
        flags |= O_CREAT;
    }
    if (options.create_new) {
        flags |= O_EXCL;
    }

    return Ok(flags);
}

} // namespace detail

class File {
public:
    File(const File&) = delete;
    auto operator=(const File&) -> File& = delete;

    File(File&&) noexcept = default;
    auto operator=(File&&) noexcept -> File& = default;

    ~File() = default;

    [[nodiscard]] static auto open(String path, OpenOptions options = OpenOptions::read_only())
        -> async::Task<Result<File>>
    {
        co_return co_await detail::spawn_blocking_result([path = std::move(path), options] {
            auto flags = detail::open_flags(options);
            if (!flags) {
                return Result<File>(Err(std::move(flags).error()));
            }

            auto fd = detail::open_fd(path, *flags, options.mode);
            if (!fd) {
                return Result<File>(Err(std::move(fd).error()));
            }

            return Ok(File(std::move(*fd)));
        });
    }

    [[nodiscard]] static auto create(String path) -> async::Task<Result<File>>
    {
        co_return co_await open(std::move(path), OpenOptions::create_truncate());
    }

    [[nodiscard]] static auto append(String path) -> async::Task<Result<File>>
    {
        co_return co_await open(std::move(path), OpenOptions::append_create());
    }

    [[nodiscard]] auto read_at(u64 offset, usize len) const -> async::Task<Result<Bytes>>
    {
        auto state = state_;
        co_return co_await detail::spawn_blocking_result([state = std::move(state), offset, len] {
            if (!state) {
                return Result<Bytes>(Err(detail::make_error(std::errc::bad_file_descriptor, "file is not open")));
            }
            std::lock_guard lock(state->mutex);
            if (state->fd < 0) {
                return Result<Bytes>(Err(detail::make_error(std::errc::bad_file_descriptor, "file is closed")));
            }
            return detail::read_at_fd_sync(state->fd, offset, len);
        });
    }

    [[nodiscard]] auto write_at(u64 offset, Bytes data) const -> async::Task<Result<usize>>
    {
        auto state = state_;
        co_return co_await detail::spawn_blocking_result([state = std::move(state),
                                                          offset,
                                                          data = std::move(data)] {
            if (!state) {
                return Result<usize>(Err(detail::make_error(std::errc::bad_file_descriptor, "file is not open")));
            }
            std::lock_guard lock(state->mutex);
            if (state->fd < 0) {
                return Result<usize>(Err(detail::make_error(std::errc::bad_file_descriptor, "file is closed")));
            }
            return detail::write_at_fd_sync(state->fd, offset, data.data(), data.size());
        });
    }

    [[nodiscard]] auto write_string_at(u64 offset, String data) const -> async::Task<Result<usize>>
    {
        auto state = state_;
        co_return co_await detail::spawn_blocking_result([state = std::move(state),
                                                          offset,
                                                          data = std::move(data)] {
            if (!state) {
                return Result<usize>(Err(detail::make_error(std::errc::bad_file_descriptor, "file is not open")));
            }
            std::lock_guard lock(state->mutex);
            if (state->fd < 0) {
                return Result<usize>(Err(detail::make_error(std::errc::bad_file_descriptor, "file is closed")));
            }
            return detail::write_at_fd_sync(state->fd,
                                            offset,
                                            reinterpret_cast<const u8*>(data.data()),
                                            data.size());
        });
    }

    [[nodiscard]] auto metadata() const -> async::Task<Result<Metadata>>
    {
        auto state = state_;
        co_return co_await detail::spawn_blocking_result([state = std::move(state)] {
            if (!state) {
                return Result<Metadata>(Err(detail::make_error(std::errc::bad_file_descriptor, "file is not open")));
            }
            std::lock_guard lock(state->mutex);
            if (state->fd < 0) {
                return Result<Metadata>(Err(detail::make_error(std::errc::bad_file_descriptor, "file is closed")));
            }
            return detail::metadata_fd_sync(state->fd);
        });
    }

    [[nodiscard]] auto close() -> Result<Unit>
    {
        if (!state_) {
            return Ok();
        }
        return state_->close();
    }

    [[nodiscard]] auto valid() const -> bool
    {
        if (!state_) {
            return false;
        }
        std::lock_guard lock(state_->mutex);
        return state_->fd >= 0;
    }

    [[nodiscard]] auto fd() const -> i32
    {
        if (!state_) {
            return -1;
        }
        std::lock_guard lock(state_->mutex);
        return state_->fd;
    }

private:
    struct State {
        explicit State(i32 file_fd) : fd(file_fd) { }

        State(const State&) = delete;
        auto operator=(const State&) -> State& = delete;

        ~State() { (void)close(); }

        [[nodiscard]] auto close() -> Result<Unit>
        {
            std::lock_guard lock(mutex);
            if (fd < 0) {
                return Ok();
            }

            const i32 close_fd = std::exchange(fd, -1);
            if (::close(close_fd) < 0) {
                return Err(SystemError::from_errno());
            }
            return Ok();
        }

        mutable std::mutex mutex;
        i32 fd = -1;
    };

    explicit File(detail::OwnedFd fd) : state_(std::make_shared<State>(fd.release())) { }

    Shared<State> state_;
};

} // namespace rain::fs
