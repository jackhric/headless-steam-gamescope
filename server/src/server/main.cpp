#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <helpers/logger.hpp>
#include <sys/wait.h>
#include <memory>
#include <mutex>
#include <server/control.hpp>
#include <server/encoder_config.hpp>
#include <server/media.hpp>
#include <server/rtsp_server.hpp>
#include <server/servers.hpp>
#include <server/session.hpp>
#include <server/state.hpp>
#include <server/uinput.hpp>
#include <thread>

static std::string env_or(const char *k, const std::string &def) {
  const char *v = std::getenv(k);
  return v ? std::string(v) : def;
}

// PID 1 must reap SIGCHLD or launched app trees pile up as zombies.
static void reap_children(int) {
  int saved = errno;
  while (::waitpid(-1, nullptr, WNOHANG) > 0) {
  }
  errno = saved;
}

int main() {
  {
    struct sigaction sa{};
    sa.sa_handler = reap_children;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    ::sigaction(SIGCHLD, &sa, nullptr);
  }

  logs::init(logs::parse_level(env_or("STEAM_STREAM_LOG_LEVEL", "INFO")));

  auto state_dir = env_or("STEAM_STREAM_STATE_DIR", "/var/lib/steam-stream");
  auto state = state::AppState::init(state_dir);
  state.http_port = std::stoi(env_or("STEAM_STREAM_HTTP_PORT", "47989"));
  state.https_port = std::stoi(env_or("STEAM_STREAM_HTTPS_PORT", "47984"));
  state.rtsp_port = std::stoi(env_or("STEAM_STREAM_RTSP_PORT", "48010"));
  auto render_node = env_or("STEAM_STREAM_RENDER_NODE", "/dev/dri/renderD129");

  logs::log(logs::info, "steam-stream-server starting: host={} uuid={} state_dir={}",
            state.hostname, state.uuid, state_dir);

  media::MediaSession::global_init();

  // Seed the host-mounted encoder config now (if absent) so it exists for editing before the
  // first stream, and log which encoders are actually available on this box.
  {
    auto cfg_path = encoder_config::config_path();
    encoder_config::create_default_if_missing(cfg_path);
    auto avail = encoder_config::available_encoders(encoder_config::load_or_seed());
    logs::log(logs::info, "[ENCODER-CFG] config={} available: h264={} hevc={} av1={}", cfg_path,
              avail.h264, avail.hevc, avail.av1);
    // Advertise HEVC/AV1 to the client only if this box can actually encode them; otherwise the
    // client's ANNOUNCE falls back to H264 (bitStreamFormat=0) even when HEVC is selected.
    state.support_hevc = avail.hevc;
    state.support_av1 = avail.av1;
  }

  if (!control::ControlServer::global_init())
    logs::log(logs::warning, "ENet init failed -- control channel will not work");
  if (!input::uinput_available())
    logs::log(logs::warning, "/dev/uinput not writable -- input injection will fail "
                             "(run container with --device /dev/uinput)");

  auto media_mtx = std::make_shared<std::mutex>();
  std::shared_ptr<media::MediaSession> media_holder;

  // id match so a stale STOP can't kill a newer session.
  auto stop_session = [&](std::size_t sid) {
    std::shared_ptr<media::MediaSession> victim;
    {
      std::lock_guard<std::mutex> lk(*media_mtx);
      if (media_holder && media_holder->session_id() == sid)
        victim.swap(media_holder);
    }
    if (victim) {
      logs::log(logs::info, "[MAIN] stopping session {} (STOP/cancel)", sid);
      victim->stop();
    }
  };
  state.stop_session = stop_session;

  control::ControlServer control_server(state.control_stream_port, state.sessions);
  control_server.set_idr_callback([&](std::size_t sid) {
    std::lock_guard<std::mutex> lk(*media_mtx);
    if (media_holder)
      media_holder->force_idr();
  });
  control_server.set_media_accessor([&]() -> std::shared_ptr<media::MediaSession> {
    std::lock_guard<std::mutex> lk(*media_mtx);
    return media_holder;
  });
  control_server.set_stop_callback(stop_session);
  std::thread control_thread([&control_server]() { control_server.run(); });

  std::thread rtsp_thread([&]() {
    rtsp::run_server(state.rtsp_port, *state.sessions, [&](session::StreamSession &s) {
      logs::log(logs::info,
                "[RTSP] PLAY for session {} -- starting media pipeline ({}x{}@{} {}kbps fec={}% "
                "audio={}ch encrypt={})",
                s.session_id, s.video.width, s.video.height, s.video.fps, s.video.bitrate_kbps,
                s.video.fec_percentage, s.audio.channels, s.audio.encrypt);
      auto sess = state.sessions->get_by_id(s.session_id);
      if (!sess) {
        logs::log(logs::error, "[RTSP] PLAY: session {} vanished from registry", s.session_id);
        return;
      }

      // RESUME: must NOT tear down/relaunch the app -- reuse the pipeline, re-target RTP at the
      // reconnected client, and force an IDR so the new decoder syncs.
      {
        std::shared_ptr<media::MediaSession> existing;
        {
          std::lock_guard<std::mutex> lk(*media_mtx);
          existing = media_holder;
        }
        if (existing && existing->session_id() == s.session_id && existing->is_active()) {
          logs::log(logs::info,
                    "[RTSP] PLAY for session {} -- RESUME: reusing running app (pgid {}); "
                    "re-targeting RTP + forcing IDR (no relaunch)",
                    s.session_id, existing->app_pid());
          // Apply the renegotiated bitrate before the IDR so the keyframe uses the new rate.
          existing->update_bitrate(s.video.bitrate_kbps, s.video.fps);
          // A resume can renegotiate the audio layout; the Opus stream config is baked into the
          // pipeline, so rebuild it or the client can't decode. The sink keeps its old layout
          // (pulse remixes) -- true surround applies once the app is next relaunched.
          if (existing->audio_channels() != s.audio.channels) {
            logs::log(logs::info,
                      "[RTSP] resume changed audio {}ch -> {}ch: rebuilding audio pipeline "
                      "(relaunch the app for native surround)",
                      existing->audio_channels(), s.audio.channels);
            existing->rebuild_audio(s);
          }
          existing->retarget();
          existing->force_idr();
          return;
        }
      }

      // FRESH LAUNCH: tear down the previous session BEFORE launching -- else the two collide on
      // the X11 display (:0 "Address already in use"). Drop the lock during the blocking teardown.
      std::shared_ptr<media::MediaSession> previous;
      {
        std::lock_guard<std::mutex> lk(*media_mtx);
        previous.swap(media_holder);
      }
      if (previous)
        previous->stop();
      previous.reset();

      logs::log(logs::info, "[RTSP] PLAY for session {} -- LAUNCH: starting pipeline + app",
                s.session_id);
      auto ms = media::MediaSession::start(sess, render_node);
      std::lock_guard<std::mutex> lk(*media_mtx);
      media_holder = ms;
    });
  });

  std::thread http_thread([&state]() { HTTPServers::start_http(state); });
  HTTPServers::start_https(state); // blocks
  http_thread.join();
  rtsp_thread.join();
  control_server.stop();
  control_thread.join();
  return 0;
}
