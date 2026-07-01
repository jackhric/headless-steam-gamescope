#pragma once

#include <functional>
#include <server/session.hpp>

namespace rtsp {

// Blocks running its io_context; run on its own thread.
void run_server(int port,
                session::SessionRegistry &registry,
                std::function<void(session::StreamSession &)> on_play = {});

} // namespace rtsp
