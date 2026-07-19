#!/usr/bin/env bash
# SteamOS-Containerized quickstart: install Docker if needed, detect the GPU,
# generate a matching docker-compose.yml, and bring the container up.
#
# Run as root:  curl -fsSL <raw-url>/quickstart.sh | bash
#
# Tunables (env vars):
#   STEAM_STREAM_IMAGE        image to pull/run  (default: see IMAGE_REF below)
#   STEAM_STREAM_INSTALL_DIR  install dir        (default: /opt/steam-stream)
#   STEAM_STREAM_GPU          force vendor: nvidia | amd | intel
#   STEAM_STREAM_RENDER_NODE  force render node, e.g. /dev/dri/renderD128
#   STEAM_STREAM_LIBRARIES    colon-separated host dirs to mount as game libraries
#                             (used instead of prompting when set or non-interactive)

set -euo pipefail

# Public image ref. Blank until the image is published; falls back to a locally
# built local/steam-stream:latest if present.
IMAGE_REF="${STEAM_STREAM_IMAGE:-}"
LOCAL_IMAGE="local/steam-stream:latest"

INSTALL_DIR="${STEAM_STREAM_INSTALL_DIR:-/opt/steam-stream}"
CONTAINER_NAME="steam-stream"
HTTP_PORT=47989
HEALTH_TIMEOUT=120

# --- helpers -----------------------------------------------------------------

info() { printf '\033[1;32m[quickstart]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[quickstart]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[quickstart]\033[0m ERROR: %s\n' "$*" >&2; exit 1; }

# Piped via curl|bash means stdin is the script itself; prompt on /dev/tty.
# Must test a real open: permission bits pass access(2) even without a
# controlling terminal (ssh -T, cron, systemd), where open() fails with ENXIO.
have_tty() { { : < /dev/tty; } 2>/dev/null; }
ask() { # ask <prompt> <varname> [default]
  local prompt="$1" var="$2" default="${3:-}" reply
  if have_tty; then
    printf '%s' "$prompt" > /dev/tty
    IFS= read -r reply < /dev/tty || reply=""
  else
    reply=""
  fi
  printf -v "$var" '%s' "${reply:-$default}"
}

pkg_family() {
  # shellcheck disable=SC1091
  . /etc/os-release 2>/dev/null || true
  case "${ID:-} ${ID_LIKE:-}" in
    *debian*|*ubuntu*) echo deb ;;
    *rhel*|*fedora*|*centos*) echo rpm ;;
    *suse*) echo suse ;;
    *arch*) echo arch ;;
    *) echo unknown ;;
  esac
}

# --- preflight ---------------------------------------------------------------

[ "$(id -u)" -eq 0 ] || die "must run as root (sudo)"
[ "$(uname -s)" = "Linux" ] || die "Linux only"
[ "$(uname -m)" = "x86_64" ] || die "x86_64 only (Steam has no native arm64 build)"

for tool in curl grep stat; do
  command -v "$tool" >/dev/null || die "missing required tool: $tool"
done

# uinput is how the container injects virtual gamepads/keyboard/mouse.
if [ ! -e /dev/uinput ]; then
  info "loading uinput kernel module"
  modprobe uinput || die "could not load uinput module (custom kernel without CONFIG_INPUT_UINPUT?)"
fi
[ -e /dev/uinput ] || die "/dev/uinput still missing after modprobe"
if [ -d /etc/modules-load.d ] && [ ! -e /etc/modules-load.d/steam-stream.conf ]; then
  echo uinput > /etc/modules-load.d/steam-stream.conf
  info "persisted uinput autoload (/etc/modules-load.d/steam-stream.conf)"
fi

[ -d /dev/dri ] || die "/dev/dri missing — no GPU driver with DRM support is loaded"
ls /dev/dri/renderD* >/dev/null 2>&1 || die "no /dev/dri/renderD* nodes — GPU driver lacks a render node"

# --- docker ------------------------------------------------------------------

if ! command -v docker >/dev/null; then
  info "Docker not found — installing"
  case "$(pkg_family)" in
    # get.docker.com does not support Arch or openSUSE.
    arch) pacman -Syu --noconfirm docker docker-compose || die "Docker install failed" ;;
    suse) zypper --non-interactive install docker docker-compose || die "Docker install failed" ;;
    *)    curl -fsSL https://get.docker.com | sh || die "Docker install failed" ;;
  esac
fi
if ! docker info >/dev/null 2>&1; then
  info "starting Docker daemon"
  systemctl enable --now docker 2>/dev/null || true
  docker info >/dev/null 2>&1 || die "Docker daemon is not running and could not be started"
fi
docker compose version >/dev/null 2>&1 || die "docker compose plugin missing (install docker-compose-plugin)"

# --- GPU detection -----------------------------------------------------------

# Map render nodes to vendors via PCI vendor id.
declare -A NODE_VENDOR=()
for node in /dev/dri/renderD*; do
  sysfs="/sys/class/drm/$(basename "$node")/device/vendor"
  [ -r "$sysfs" ] || continue
  case "$(cat "$sysfs")" in
    0x10de) NODE_VENDOR[$node]=nvidia ;;
    0x1002) NODE_VENDOR[$node]=amd ;;
    0x8086) NODE_VENDOR[$node]=intel ;;
  esac
done
[ "${#NODE_VENDOR[@]}" -gt 0 ] || die "no recognized GPU (NVIDIA/AMD/Intel) behind /dev/dri"

GPU_VENDOR="${STEAM_STREAM_GPU:-}"
if [ -z "$GPU_VENDOR" ]; then
  # Prefer discrete: nvidia > amd > intel.
  for pref in nvidia amd intel; do
    for node in "${!NODE_VENDOR[@]}"; do
      [ "${NODE_VENDOR[$node]}" = "$pref" ] && GPU_VENDOR="$pref" && break 2
    done
  done
  vendors_present="$(printf '%s\n' "${NODE_VENDOR[@]}" | sort -u | tr '\n' ' ')"
  if [ "$(printf '%s\n' "${NODE_VENDOR[@]}" | sort -u | wc -l)" -gt 1 ] && have_tty; then
    ask "Multiple GPUs found (${vendors_present}) — which to use? [${GPU_VENDOR}]: " GPU_VENDOR "$GPU_VENDOR"
  fi
fi
case "$GPU_VENDOR" in nvidia|amd|intel) ;; *) die "unknown GPU vendor '$GPU_VENDOR'" ;; esac

RENDER_NODE="${STEAM_STREAM_RENDER_NODE:-}"
if [ -z "$RENDER_NODE" ]; then
  for node in $(printf '%s\n' "${!NODE_VENDOR[@]}" | sort); do
    [ "${NODE_VENDOR[$node]}" = "$GPU_VENDOR" ] && RENDER_NODE="$node" && break
  done
fi
[ -e "$RENDER_NODE" ] || die "render node $RENDER_NODE does not exist"
info "GPU: $GPU_VENDOR ($RENDER_NODE)"

# --- NVIDIA container toolkit + CDI ------------------------------------------

if [ "$GPU_VENDOR" = "nvidia" ]; then
  [ -e /proc/driver/nvidia/version ] || command -v nvidia-smi >/dev/null \
    || die "NVIDIA GPU selected but no NVIDIA driver is loaded — install the proprietary driver first"

  if ! command -v nvidia-ctk >/dev/null; then
    info "installing NVIDIA Container Toolkit"
    case "$(pkg_family)" in
      deb)
        curl -fsSL https://nvidia.github.io/libnvidia-container/gpgkey \
          | gpg --batch --yes --dearmor -o /usr/share/keyrings/nvidia-container-toolkit-keyring.gpg
        curl -fsSL https://nvidia.github.io/libnvidia-container/stable/deb/nvidia-container-toolkit.list \
          | sed 's#deb https://#deb [signed-by=/usr/share/keyrings/nvidia-container-toolkit-keyring.gpg] https://#' \
          > /etc/apt/sources.list.d/nvidia-container-toolkit.list
        apt-get update -qq || die "apt update failed"
        apt-get install -y -qq nvidia-container-toolkit ;;
      rpm)
        curl -fsSL https://nvidia.github.io/libnvidia-container/stable/rpm/nvidia-container-toolkit.repo \
          > /etc/yum.repos.d/nvidia-container-toolkit.repo
        dnf install -y -q nvidia-container-toolkit ;;
      suse)
        zypper --non-interactive ar https://nvidia.github.io/libnvidia-container/stable/rpm/nvidia-container-toolkit.repo || true
        zypper --gpg-auto-import-keys --non-interactive install nvidia-container-toolkit ;;
      arch)
        # -Syu, not -Sy: installing against refreshed DBs without upgrading is a partial upgrade.
        pacman -Syu --noconfirm nvidia-container-toolkit ;;
      *)
        die "unknown distro — install nvidia-container-toolkit manually, then re-run" ;;
    esac
  fi

  # The compose 'nvidia.com/gpu=all' device syntax needs Engine >= 25 and compose >= 2.24
  # (a preinstalled distro Docker, e.g. Ubuntu 22.04's, can be older).
  engine_ver="$(docker version -f '{{.Server.Version}}' 2>/dev/null || echo 0)"
  engine_major="${engine_ver%%.*}"
  case "$engine_major" in *[!0-9]*|'') engine_major=0 ;; esac
  [ "$engine_major" -ge 25 ] || die "Docker Engine $engine_ver is too old for CDI GPU passthrough (need >= 25) — upgrade Docker"
  compose_ver="$(docker compose version --short 2>/dev/null | tr -d v)"
  if [ "$(printf '%s\n' "2.24.0" "$compose_ver" | sort -V | head -n1)" != "2.24.0" ]; then
    die "docker compose $compose_ver is too old for CDI device syntax (need >= 2.24) — upgrade the compose plugin"
  fi

  info "generating CDI spec (/etc/cdi/nvidia.yaml)"
  mkdir -p /etc/cdi
  nvidia-ctk cdi generate --output=/etc/cdi/nvidia.yaml || die "CDI spec generation failed"

  # Docker <28 needs CDI explicitly enabled; setting it is harmless on newer versions.
  DAEMON_JSON=/etc/docker/daemon.json
  if ! grep -qs '"cdi"[[:space:]]*:[[:space:]]*true' "$DAEMON_JSON" 2>/dev/null; then
    if command -v python3 >/dev/null; then
      if python3 - "$DAEMON_JSON" <<'EOF'
import json, os, sys
path = sys.argv[1]
cfg = {}
if os.path.exists(path):
    with open(path) as f:
        text = f.read()
    if text.strip():
        try:
            cfg = json.loads(text)
        except ValueError:
            sys.exit(3)
cfg.setdefault("features", {})["cdi"] = True
os.makedirs(os.path.dirname(path), exist_ok=True)
with open(path, "w") as f:
    json.dump(cfg, f, indent=2)
EOF
      then
        info "enabled CDI in $DAEMON_JSON — restarting Docker"
        systemctl restart docker || die "docker restart failed"
      else
        warn "$DAEMON_JSON is not valid JSON — add {\"features\":{\"cdi\":true}} yourself if the container fails to see the GPU"
      fi
    elif [ ! -e "$DAEMON_JSON" ]; then
      mkdir -p /etc/docker
      printf '{\n  "features": { "cdi": true }\n}\n' > "$DAEMON_JSON"
      info "enabled CDI in $DAEMON_JSON — restarting Docker"
      systemctl restart docker || die "docker restart failed"
    else
      warn "cannot safely edit existing $DAEMON_JSON without python3 — add {\"features\":{\"cdi\":true}} yourself if the container fails to see the GPU"
    fi
  fi
fi

# --- device group IDs ----------------------------------------------------------

# GIDs differ per distro; derive them from the actual device nodes.
GIDS=()
add_gid() {
  local gid="$1"
  [ -n "$gid" ] && [ "$gid" != 0 ] || return 0
  local g
  for g in "${GIDS[@]:-}"; do [ "$g" = "$gid" ] && return 0; done
  GIDS+=("$gid")
}
add_gid "$(stat -c %g "$RENDER_NODE")"
card="$(ls /dev/dri/card* 2>/dev/null | head -n1 || true)"
[ -n "$card" ] && add_gid "$(stat -c %g "$card")"
event="$(ls /dev/input/event* 2>/dev/null | head -n1 || true)"
if [ -n "$event" ]; then
  add_gid "$(stat -c %g "$event")"
else
  add_gid "$(getent group input | cut -d: -f3)"
fi
[ "${#GIDS[@]}" -gt 0 ] || warn "could not derive render/video/input GIDs — GPU/input access may fail in the container"

# --- game library mounts -------------------------------------------------------

LIBRARY_MOUNTS=()
if [ -n "${STEAM_STREAM_LIBRARIES:-}" ]; then
  IFS=':' read -ra libs <<< "$STEAM_STREAM_LIBRARIES"
  for lib in "${libs[@]}"; do
    [ -n "$lib" ] || continue
    [ -d "$lib" ] || die "library path $lib does not exist"
    LIBRARY_MOUNTS+=("$lib")
  done
elif have_tty; then
  info "Optional: mount host directories as game/ROM libraries inside the container."
  while :; do
    ask "Library path (empty to finish): " lib ""
    [ -n "$lib" ] || break
    if [ ! -d "$lib" ]; then
      ask "$lib does not exist — create it? [y/N]: " mk "n"
      # shellcheck disable=SC2154  # mk is assigned by ask() via printf -v
      case "$mk" in y|Y) mkdir -p "$lib" ;; *) continue ;; esac
    fi
    LIBRARY_MOUNTS+=("$lib")
  done
fi

# --- generate docker-compose.yml -----------------------------------------------

mkdir -p "$INSTALL_DIR/home" "$INSTALL_DIR/config"
COMPOSE="$INSTALL_DIR/docker-compose.yml"
if [ -e "$COMPOSE" ]; then
  bak="$COMPOSE.bak.$(date +%s)"
  cp "$COMPOSE" "$bak"
  warn "existing $COMPOSE backed up to $bak"
fi

# Resolve which image to run.
if [ -z "$IMAGE_REF" ]; then
  if docker image inspect "$LOCAL_IMAGE" >/dev/null 2>&1; then
    IMAGE_REF="$LOCAL_IMAGE"
    warn "no public image configured — using locally built $LOCAL_IMAGE"
  else
    die "no public image published yet and no local $LOCAL_IMAGE found.
  Either build the image from the repo (docker compose build) or set STEAM_STREAM_IMAGE=<ref> and re-run."
  fi
fi

{
  cat <<EOF
# Generated by quickstart.sh — GPU: $GPU_VENDOR
services:
  steam-stream:
    image: $IMAGE_REF
    container_name: $CONTAINER_NAME
    restart: unless-stopped
    network_mode: host

    devices:
EOF
  [ "$GPU_VENDOR" = "nvidia" ] && echo "      - nvidia.com/gpu=all"
  cat <<EOF
      - /dev/uinput:/dev/uinput
      - /dev/dri:/dev/dri

    device_cgroup_rules:
      - "c 13:* rmw"

    environment:
EOF
  if [ "$GPU_VENDOR" = "nvidia" ]; then
    cat <<EOF
      NVIDIA_VISIBLE_DEVICES: all
      NVIDIA_DRIVER_CAPABILITIES: all
      WOLF_USE_ZERO_COPY: "TRUE"
EOF
  else
    # Zero-copy is CUDA-only; VA-API uses the plain capture path.
    echo '      WOLF_USE_ZERO_COPY: "FALSE"'
  fi
  cat <<EOF
      STEAM_STREAM_RENDER_NODE: $RENDER_NODE
      WOLF_RENDER_NODE: $RENDER_NODE
      STEAM_STREAM_ENCODER_CONFIG: /config/encoders.toml
EOF
  # A bare 'group_add:' key (null) is rejected by compose — omit it entirely when empty.
  if [ "${#GIDS[@]}" -gt 0 ]; then
    echo "    group_add:"
    for gid in "${GIDS[@]}"; do
      echo "      - \"$gid\""
    done
  fi
  cat <<EOF
    shm_size: "2gb"
    security_opt:
      - seccomp=unconfined
      - apparmor=unconfined
    cap_add:
      - SYS_ADMIN
      - SYS_NICE
      - SYS_PTRACE
      - NET_RAW
      - MKNOD
      - NET_ADMIN
    # Steam/CEF use host IPC (shared-memory segments) for the webhelper <-> client transport.
    ipc: host
    ulimits:
      nofile:
        soft: 10240
        hard: 10240

    volumes:
      # Steam login/library + pairing state, persisted on the host.
      - $INSTALL_DIR/home:/home/retro
      # Encoder config, seeded by the server on first run if absent.
      - $INSTALL_DIR/config:/config
      - /dev/input:/dev/input
EOF
  for lib in "${LIBRARY_MOUNTS[@]:-}"; do
    [ -n "$lib" ] && echo "      - $lib:$lib"
  done
} > "$COMPOSE"
info "wrote $COMPOSE"

# --- pull + run + health check ---------------------------------------------------

if [ "$IMAGE_REF" != "$LOCAL_IMAGE" ]; then
  info "pulling $IMAGE_REF"
  docker pull "$IMAGE_REF" || die "image pull failed"
fi

info "starting container"
docker compose -f "$COMPOSE" up -d

info "waiting for the server to come up (http://localhost:$HTTP_PORT, ${HEALTH_TIMEOUT}s timeout)"
deadline=$(( $(date +%s) + HEALTH_TIMEOUT ))
healthy=0
while [ "$(date +%s)" -lt "$deadline" ]; do
  state="$(docker inspect -f '{{.State.Status}}' "$CONTAINER_NAME" 2>/dev/null || echo missing)"
  if [ "$state" != "running" ]; then
    warn "container state: $state — recent logs:"
    docker logs --tail 30 "$CONTAINER_NAME" 2>&1 | sed 's/^/    /' || true
    die "container is not running"
  fi
  if curl -fsS -m 3 "http://127.0.0.1:$HTTP_PORT/serverinfo" >/dev/null 2>&1 \
     || curl -fsS -m 3 "http://127.0.0.1:$HTTP_PORT/" >/dev/null 2>&1; then
    healthy=1
    break
  fi
  sleep 2
done

if [ "$healthy" -ne 1 ]; then
  warn "server did not answer on port $HTTP_PORT within ${HEALTH_TIMEOUT}s — recent logs:"
  docker logs --tail 50 "$CONTAINER_NAME" 2>&1 | sed 's/^/    /' || true
  die "health check failed"
fi

# hostname -I is not universal (Arch ships inetutils hostname without it).
host_ip="$(hostname -I 2>/dev/null | awk '{print $1}' || true)"
[ -n "$host_ip" ] || host_ip="$(ip -4 route get 1 2>/dev/null | awk '{for(i=1;i<NF;i++) if($i=="src") print $(i+1); exit}' || true)"
pin_url="$(docker logs "$CONTAINER_NAME" 2>&1 | grep -o 'http://[^ ]*/pin/#[a-zA-Z0-9]*' | tail -n1 || true)"

info "steam-stream is up!"
cat <<EOF

  Next steps:
    1. Open Moonlight on your client and add this host: ${host_ip:-<this-machine>}
    2. When Moonlight shows a PIN, enter it at:
       ${pin_url:-http://${host_ip:-localhost}:$HTTP_PORT/pin/  (exact URL with secret: docker logs $CONTAINER_NAME | grep pin)}

  Install dir:     $INSTALL_DIR
  Compose file:    $COMPOSE
  Steam data:      $INSTALL_DIR/home
  Encoder config:  $INSTALL_DIR/config/encoders.toml (seeded on first stream if absent)
  Logs:            docker logs -f $CONTAINER_NAME
EOF
