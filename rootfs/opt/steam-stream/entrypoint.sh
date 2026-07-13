#!/usr/bin/env bash
# steam-stream container entrypoint: GOW cont-init.d as root, then PulseAudio as retro,
# then exec the server (root, setuid-drops to retro per app launch).
set -uo pipefail

source /opt/gow/bash-lib/utils.sh

PUID="${PUID:-1000}"
PGID="${PGID:-1000}"
UNAME="${UNAME:-retro}"
HOME="${HOME:-/home/retro}"
# A private runtime dir we own end-to-end; created before cont-init so 10-setup_user can chown it.
export XDG_RUNTIME_DIR="/tmp/sockets"
mkdir -p "${XDG_RUNTIME_DIR}/pulse"

# GOW container init (root only); guard each script so a non-essential failure never aborts boot.
if [ "$(id -u)" = "0" ]; then
  for init_script in /etc/cont-init.d/*.sh; do
    gow_log "[entrypoint] cont-init: ${init_script}"
    # shellcheck source=/dev/null
    source "${init_script}" || gow_log "[entrypoint] WARN: ${init_script} returned non-zero (continuing)"
  done
fi

# libgbm needs nvidia-drm_gbm.so for GPU glamor, but GOW's 30-nvidia.sh only looks for the
# allocator under /usr/lib/x86_64-linux-gnu/ while CDI injects it elsewhere; resolve + symlink it.
if [ "$(id -u)" = "0" ]; then
  GBM_DIR=/usr/lib/x86_64-linux-gnu/gbm
  if [ ! -e "${GBM_DIR}/nvidia-drm_gbm.so" ]; then
    ALLOC="$(ldconfig -p 2>/dev/null | awk '/libnvidia-allocator\.so\.1/{print $NF; exit}')"
    if [ -n "${ALLOC}" ] && [ -e "${ALLOC}" ]; then
      mkdir -p "${GBM_DIR}"
      ln -sf "${ALLOC}" "${GBM_DIR}/nvidia-drm_gbm.so"
      gow_log "[entrypoint] created GBM backend ${GBM_DIR}/nvidia-drm_gbm.so -> ${ALLOC}"
    else
      gow_log "[entrypoint] WARN: libnvidia-allocator.so.1 not found; Xwayland glamor may be software-only"
    fi
  fi
fi

chown -R "${PUID}:${PGID}" "${XDG_RUNTIME_DIR}"
chmod 700 "${XDG_RUNTIME_DIR}"

# Clear stale X sockets a hard-killed Xwayland may have left; sticky dir so uid retro can rebind.
mkdir -p /tmp/.X11-unix
chmod 1777 /tmp/.X11-unix
rm -f /tmp/.X11-unix/X* 2>/dev/null || true

mkdir -p "${HOME}/.steam-stream"
chown "${PUID}:${PGID}" "${HOME}/.steam-stream"

# No udevd runs in-container, so the server injects fake-udev hotplug events + hwdb entries for its
# virtual gamepad (see fake_udev.cpp). SDL/Steam (uid retro) read /run/udev/data to classify the
# pad; the server (root) writes it. World-rwx so both sides work regardless of uid.
mkdir -p /run/udev/data
chmod -R 0777 /run/udev

# PULSE_SERVER must be unset while the daemon starts (it refuses to autospawn otherwise);
# exported only once the daemon is up.
PULSE_SOCK="unix:${XDG_RUNTIME_DIR}/pulse/native"
gow_log "[entrypoint] starting PulseAudio + steam-stream null sink as ${UNAME}"
gosu "${UNAME}" env XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR}" PULSE_SERVER= \
  pulseaudio --start --exit-idle-time=-1 --log-target=stderr \
  || gow_log "[entrypoint] WARN: pulseaudio --start returned non-zero"
for _ in $(seq 1 20); do
  gosu "${UNAME}" env XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR}" pactl info >/dev/null 2>&1 && break
  sleep 0.5
done
# Default stereo sink so audio works before the first stream; the server re-runs pulse-sink.sh
# with the client's negotiated channel count (2/6/8) at each fresh session launch.
gosu "${UNAME}" env XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR}" \
  /opt/steam-stream/pulse-sink.sh 2 \
  || gow_log "[entrypoint] WARN: could not create steam-stream null sink"
gow_log "[entrypoint] pulse sinks: $(gosu "${UNAME}" env XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR}" pactl list short sinks 2>/dev/null | tr '\n' ';')"

export PULSE_SERVER="${PULSE_SOCK}"
export PULSE_SINK="steam-stream"

# Hand off to the server (runs as root, drops to retro per launch).
export GST_PLUGIN_PATH="${GST_PLUGIN_PATH:-/usr/local/lib/x86_64-linux-gnu/gstreamer-1.0}"
export LD_LIBRARY_PATH="/usr/local/nvidia/lib:/usr/local/nvidia/lib64:${LD_LIBRARY_PATH:-}"
export STEAM_STREAM_RENDER_NODE="${STEAM_STREAM_RENDER_NODE:-/dev/dri/renderD129}"
export STEAM_STREAM_STATE_DIR="${STEAM_STREAM_STATE_DIR:-${HOME}/.steam-stream}"
export STEAM_STREAM_RUN_UID="${PUID}" STEAM_STREAM_RUN_GID="${PGID}" STEAM_STREAM_RUN_USER="${UNAME}"
export STEAM_STREAM_HOME="${HOME}"

gow_log "[entrypoint] exec steam-stream-server (as root; setuid-drops to ${UNAME} per launch)"
exec steam-stream-server
