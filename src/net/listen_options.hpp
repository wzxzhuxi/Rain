#pragma once

#include "core/types.hpp"

namespace rain::net {

struct ListenOptions {
    u32 backlog = 128;
    bool reuse_addr = true;
    bool reuse_port = true;
};

} // namespace rain::net
