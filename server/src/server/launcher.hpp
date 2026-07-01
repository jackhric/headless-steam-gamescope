#pragma once

#include <server/session.hpp>
#include <string>
#include <sys/types.h>

namespace launcher {

// Fork + exec the launch script for this session's app. Returns the child pid, or -1 on failure.
pid_t launch_app(const session::StreamSession &s, const std::string &wayland_display);

} // namespace launcher
