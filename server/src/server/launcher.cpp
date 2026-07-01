#include <server/launcher.hpp>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <grp.h>
#include <helpers/logger.hpp>
#include <pwd.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace launcher {

namespace {

const char *env_or(const char *k, const char *def) {
  const char *v = std::getenv(k);
  return (v && *v) ? v : def;
}

struct LaunchSpec {
  std::string gow_var; // "RUN_GAMESCOPE=1" or "RUN_SWAY=1"
  std::string label;
  bool is_sway = false;
};

// Keyed on appid with a name fallback so it stays correct if the IDs are reordered.
LaunchSpec spec_for(const session::StreamSession &s) {
  bool desktop = s.app_id == "2" || s.app_name.find("Desktop") != std::string::npos;
  if (desktop)
    return {"RUN_SWAY=1", "Steam Desktop (sway)", true};
  return {"RUN_GAMESCOPE=1", "Steam Big Picture (gamescope)", false};
}

void push_passthrough(std::vector<std::string> &env, const char *key) {
  if (const char *v = std::getenv(key))
    env.push_back(std::string(key) + "=" + v);
}

} // namespace

pid_t launch_app(const session::StreamSession &s, const std::string &wayland_display) {
  auto spec = spec_for(s);

  std::string script = env_or("STEAM_STREAM_LAUNCH_SCRIPT", "/opt/gow/startup-app.sh");
  if (::access(script.c_str(), R_OK) != 0) {
    logs::log(logs::warning,
              "[LAUNCH] launch script {} not found -- skipping real app launch (set "
              "STEAM_STREAM_TEST_CLIENT for a stand-in, or run in the steam-stream image)",
              script);
    return -1;
  }

  uid_t uid = static_cast<uid_t>(std::stoul(env_or("STEAM_STREAM_RUN_UID", "1000")));
  gid_t gid = static_cast<gid_t>(std::stoul(env_or("STEAM_STREAM_RUN_GID", "1000")));
  std::string home = env_or("STEAM_STREAM_HOME", "/home/retro");
  std::string xdg = env_or("XDG_RUNTIME_DIR", "/tmp/sockets");
  std::string user = env_or("STEAM_STREAM_RUN_USER", "retro");

  int w = s.video.width > 0 ? s.video.width : (s.client_width > 0 ? s.client_width : 1920);
  int h = s.video.height > 0 ? s.video.height : (s.client_height > 0 ? s.client_height : 1080);
  int fps = s.video.fps > 0 ? s.video.fps : (s.client_fps > 0 ? s.client_fps : 60);

  logs::log(logs::info,
            "[LAUNCH] session {} app='{}' (id={}) -> {} into WAYLAND_DISPLAY={} "
            "{}x{}@{} as uid {} (script {})",
            s.session_id, s.app_name, s.app_id, spec.label, wayland_display, w, h, fps, uid,
            script);

  // The root-owned wayland socket at mode 0755 needs write perm relaxed so the retro-uid app
  // can connect (safe: per-session private dir).
  if (!wayland_display.empty() && wayland_display[0] != '/') {
    std::string sock = xdg + "/" + wayland_display;
    if (::chmod(sock.c_str(), 0777) != 0)
      logs::log(logs::warning, "[LAUNCH] chmod wayland socket {} failed: {}", sock,
                std::strerror(errno));
  }

  pid_t pid = ::fork();
  if (pid < 0) {
    logs::log(logs::error, "[LAUNCH] fork failed: {}", std::strerror(errno));
    return -1;
  }
  if (pid != 0)
    return pid; // parent

  // setsid so the whole app tree shares one process group for killpg teardown.
  ::setsid();
  if (::geteuid() == 0) {
    ::initgroups(user.c_str(), gid); // best-effort supplementary groups
    if (::setgid(gid) != 0 || ::setuid(uid) != 0)
      _exit(126);
  }

  std::vector<std::string> env = {
      "HOME=" + home,
      "USER=" + user,
      "LOGNAME=" + user,
      "XDG_RUNTIME_DIR=" + xdg,
      "WAYLAND_DISPLAY=" + wayland_display,
      spec.gow_var,
      "APPIMAGE_EXTRACT_AND_RUN=1",
      "PULSE_SINK=" + std::string(env_or("PULSE_SINK", "steam-stream")),
      fmt::format("GAMESCOPE_WIDTH={}", w),
      fmt::format("GAMESCOPE_HEIGHT={}", h),
      fmt::format("GAMESCOPE_REFRESH={}", fps),
      "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/games",
  };
  if (const char *p = std::getenv("PULSE_SERVER"); p && *p)
    env.push_back(std::string("PULSE_SERVER=") + p);

  // NVIDIA nested compositor (sway/wlroots) can't allocate GBM buffers (no DRM-master) -> fall
  // back to the pixman software renderer; gamescope unaffected. Override via WLR_RENDERER.
  if (spec.is_sway && !std::getenv("WLR_RENDERER")) {
    env.push_back("WLR_RENDERER=pixman");
    env.push_back("WLR_NO_HARDWARE_CURSORS=1");
  }

  for (const char *k : {"NVIDIA_DRIVER_CAPABILITIES", "NVIDIA_VISIBLE_DEVICES",
                        "LD_LIBRARY_PATH", "WLR_RENDERER", "WLR_NO_HARDWARE_CURSORS",
                        "STEAM_STARTUP_FLAGS", "GAMESCOPE_MODE", "PROTON_LOG", "DISPLAY"})
    push_passthrough(env, k);

  std::vector<char *> envp;
  envp.reserve(env.size() + 1);
  for (auto &e : env)
    envp.push_back(const_cast<char *>(e.c_str()));
  envp.push_back(nullptr);

  if (::chdir(home.c_str()) != 0) { /* non-fatal */
  }
  ::execle("/bin/bash", "bash", script.c_str(), static_cast<char *>(nullptr), envp.data());
  _exit(127); // exec failed
}

} // namespace launcher
