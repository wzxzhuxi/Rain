// Rain compile check — verifies all headers compile cleanly.

#include "core/types.hpp"
#include "core/result.hpp"
#include "core/concepts.hpp"

#include "sync/channel.hpp"
#include "sync/spinlock.hpp"

#include "async/concepts.hpp"
#include "async/task.hpp"
#include "async/timer.hpp"
#include "async/io_reactor.hpp"
#include "async/signal.hpp"
#include "async/event_loop.hpp"
#include "async/combinators.hpp"

#include "runtime/thread_pool.hpp"
#include "runtime/bridge.hpp"
#include "runtime/executor.hpp"
#include "runtime/runtime.hpp"

#include "net/address.hpp"
#include "net/tcp_listener.hpp"
#include "net/tcp_stream.hpp"

auto main() -> int { return 0; }
