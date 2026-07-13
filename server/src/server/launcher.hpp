#pragma once

#include <server/session.hpp>
#include <string>
#include <sys/types.h>

namespace launcher {

// Fork + exec the launch script for this session's app. Returns the child pid, or -1 on failure.
// pulse_sink overrides the PULSE_SINK handed to the app (empty -> env/default).
pid_t launch_app(const session::StreamSession &s, const std::string &wayland_display,
                 const std::string &pulse_sink = "");

// (Re)create the pulse null sink with the negotiated channel count (runs pulse-sink.sh as the
// pulse user). Failure is non-fatal: the stream still works, just remixed to the sink's layout.
bool ensure_audio_sink(int channels, const std::string &sink_name);

} // namespace launcher
