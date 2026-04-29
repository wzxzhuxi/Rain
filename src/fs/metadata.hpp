#pragma once

#include "core/types.hpp"

#include <sys/stat.h>

namespace rain::fs {

enum class FileType : u8 {
    Regular,
    Directory,
    Symlink,
    Other,
};

struct Metadata {
    u64 size = 0;
    u32 permissions = 0;
    FileType type = FileType::Other;

    [[nodiscard]] auto is_file() const noexcept -> bool { return type == FileType::Regular; }
    [[nodiscard]] auto is_dir() const noexcept -> bool { return type == FileType::Directory; }
    [[nodiscard]] auto is_symlink() const noexcept -> bool { return type == FileType::Symlink; }
};

namespace detail {

[[nodiscard]] inline auto metadata_from_stat(const struct stat& st) -> Metadata
{
    FileType type = FileType::Other;
    if (S_ISREG(st.st_mode)) {
        type = FileType::Regular;
    } else if (S_ISDIR(st.st_mode)) {
        type = FileType::Directory;
    } else if (S_ISLNK(st.st_mode)) {
        type = FileType::Symlink;
    }

    return Metadata { .size = st.st_size > 0 ? static_cast<u64>(st.st_size) : u64 { 0 },
                      .permissions = static_cast<u32>(st.st_mode & 07777),
                      .type = type };
}

} // namespace detail

} // namespace rain::fs
