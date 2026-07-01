#pragma once

#include <server/state.hpp>

namespace HTTPServers {
// Each blocks running its asio io_service; run on separate threads.
void start_http(state::AppState &state);
void start_https(state::AppState &state);
} // namespace HTTPServers
