#!/bin/bash
set -e

source /opt/gow/bash-lib/utils.sh

gow_log "Steam startup.sh"

# kernel.core_pattern is not namespaced: an in-container segfault pipes to the HOST's
# systemd-coredump and pops a crash notification on the host desktop. Steam/gamescope
# crashes are not ours to debug -- suppress their cores entirely.
ulimit -c 0

# Recursively creating Steam necessary folders (https://github.com/ValveSoftware/steam-for-linux/issues/6492)
mkdir -p "$HOME/.steam/ubuntu12_32/steam-runtime"

# Some game fixes taken from the Steam Deck
export SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS=0

export STEAM_MANGOAPP_PRESETS_SUPPORTED=1
export STEAM_USE_MANGOAPP=1
export MANGOHUD_CONFIGFILE=$(mktemp /tmp/mangohud.XXXXXXXX)
export STEAM_MANGOAPP_HORIZONTAL_SUPPORTED=1

export STEAM_USE_DYNAMIC_VRS=1
export RADV_FORCE_VRS_CONFIG_FILE=$(mktemp /tmp/radv_vrs.XXXXXXXX)
# To expose vram info from radv
export WINEDLLOVERRIDES=dxgi=n


mkdir -p "$(dirname "$MANGOHUD_CONFIGFILE")"
echo "position=top-right" > "$MANGOHUD_CONFIGFILE"
echo "no_display" >> "$MANGOHUD_CONFIGFILE"

# Prepare our initial VRS config file
# for dynamic VRS in Mesa.
mkdir -p "$(dirname "$RADV_FORCE_VRS_CONFIG_FILE")"
# By default don't do half shading
echo "1x1" > "$RADV_FORCE_VRS_CONFIG_FILE"

export STEAM_GAMESCOPE_FANCY_SCALING_SUPPORT=1
export SRT_URLOPEN_PREFER_STEAM=1

export QT_IM_MODULE=steam
export GTK_IM_MODULE=Steam


if [ -n "$RUN_GAMESCOPE" ]; then
  GAMESCOPE_BIN="$(command -v gamescope)"

  STEAM_STARTUP_FLAGS=${STEAM_STARTUP_FLAGS:-"-gamepadui -steamos3 -steamdeck -steampal"}

  # Per-game Xwayland isolation (Bazzite/Deck run 2). gamescope only exports
  # STEAM_MULTIPLE_XWAYLANDS to its own children and Steam is launched separately
  # below, so export it here.
  GAMESCOPE_XWAYLAND_COUNT=${GAMESCOPE_XWAYLAND_COUNT:-2}
  if [ "$GAMESCOPE_XWAYLAND_COUNT" -gt 1 ]; then
    export STEAM_MULTIPLE_XWAYLANDS=1
  fi

  # Advertise the WSI dynamic fps limiter: Steam writes the per-game limit into
  # GAMESCOPE_LIMITER_FILE and the gamescope WSI vulkan layer inside the game paces
  # frames. Without these Steam falls back to gamescope's refresh-based limiter,
  # which is broken in nested sessions (gamescope#1479). gamescope only exports
  # them to its own children, and Steam is launched separately below.
  export STEAM_GAMESCOPE_DYNAMIC_FPSLIMITER=1
  export GAMESCOPE_LIMITER_FILE=$(mktemp /tmp/gamescope-limiter.XXXXXXXX)
  export STEAM_GAMESCOPE_NIS_SUPPORTED=1
  # The limiter is enforced by the WSI layer forcing FIFO inside the game; the
  # layer is implicit but gated on this variable, which gamescope likewise only
  # exports to its own children.
  export ENABLE_GAMESCOPE_WSI=1

  export XCURSOR_THEME=${XCURSOR_THEME:-Adwaita}
  mkdir -p "$HOME/.config/gtk-3.0"
  if ! grep -qs "gtk-cursor-theme-name" "$HOME/.config/gtk-3.0/settings.ini"; then
    printf '[Settings]\ngtk-cursor-theme-name=%s\ngtk-cursor-theme-size=32\n' \
      "$XCURSOR_THEME" >> "$HOME/.config/gtk-3.0/settings.ini"
  fi

  export STEAM_DISABLE_MANGOAPP_ATOM_WORKAROUND=1


  tmpdir="$([[ -n ${XDG_RUNTIME_DIR+x} ]] && mktemp -p "$XDG_RUNTIME_DIR" -d -t gamescope.XXXXXXX)"
  socket="${tmpdir:+$tmpdir/startup.socket}"
  stats="${tmpdir:+$tmpdir/stats.pipe}"
  # Fail early if we don't have a proper runtime directory setup
  if [[ -z $tmpdir || -z ${XDG_RUNTIME_DIR+x} ]]; then
	echo >&2 "!! Failed to find run directory in which to create stats session sockets (is \$XDG_RUNTIME_DIR set?)"
	exit 0
  fi

  export GAMESCOPE_STATS="$stats"
  mkfifo -- "$stats"
  mkfifo -- "$socket"

  linkname="gamescope-stats"
  #   shellcheck disable=SC2031 # (broken warning)
  sessionlink="${XDG_RUNTIME_DIR:+$XDG_RUNTIME_DIR/}${linkname}" 
  lockfile="$sessionlink".lck
  exec 9>"$lockfile" 
  if flock -n 9 && rm -f "$sessionlink" && ln -sf "$tmpdir" "$sessionlink"; then
	echo >&2 "Claimed global gamescope stats session at \"$sessionlink\""
  else
	echo >&2 "!! Failed to claim global gamescope stats session"
  fi

  GAMESCOPE_WIDTH=${GAMESCOPE_WIDTH:-1920}
  GAMESCOPE_HEIGHT=${GAMESCOPE_HEIGHT:-1080}
  GAMESCOPE_REFRESH=${GAMESCOPE_REFRESH:-60}
  GAMESCOPE_MODE=${GAMESCOPE_MODE:-"-b"}

  # shellcheck disable=SC2086
  # -w/-h pin the internal (XWayland) mode list to the output resolution so
  # games don't pick a standard preset (e.g. 1440p) that gamescope then
  # letterboxes into a non-16:9 output.
  # --hide-cursor-delay/--fade-out-duration match Bazzite's session: cursor shows on
  # mouse movement, auto-hides after idle.
  "${GAMESCOPE_BIN}" -e ${GAMESCOPE_MODE} --xwayland-count "${GAMESCOPE_XWAYLAND_COUNT}" --hide-cursor-delay 3000 --fade-out-duration 200 -R $socket -T $stats -W "${GAMESCOPE_WIDTH}" -H "${GAMESCOPE_HEIGHT}" -w "${GAMESCOPE_WIDTH}" -h "${GAMESCOPE_HEIGHT}" -r "${GAMESCOPE_REFRESH}" &

  # Read the variables we need from the socket
  if read -r -t 3 response_x_display response_wl_display <> "$socket"; then
	export DISPLAY="$response_x_display"
	export GAMESCOPE_WAYLAND_DISPLAY="$response_wl_display"
	unset WAYLAND_DISPLAY
	# We're done!
  else
	echo "gamescope failed"
	exit 1
  fi

  # Start IBus to enable showing the steam on-screen keyboard
  /usr/bin/ibus-daemon -d -r --panel=disable --emoji-extension=disable
  # Launch mango.
  mangoapp &

  # gamescope's Xwaylands only admit the session user (localuser auth); grant the
  # root-owned server access so PointerSync can poll the cursor. Retried briefly:
  # the second Xwayland can come up slightly after the handshake.
  (
    for attempt in 1 2 3; do
      for xs in /tmp/.X11-unix/X*; do
        [ -S "$xs" ] && DISPLAY=":${xs##*/X}" xhost +si:localuser:root >/dev/null 2>&1
      done
      sleep 2
    done
  ) &

  # Start Steam
  # shellcheck disable=SC2086
  dbus-run-session -- /usr/games/steam ${STEAM_STARTUP_FLAGS}

elif [ -n "$RUN_SWAY" ]; then
  # Desktop session: plain windowed Steam, no Big Picture.
  STEAM_STARTUP_FLAGS=${STEAM_STARTUP_FLAGS:-""}

  /usr/bin/ibus-daemon -d -r --panel=disable --emoji-extension=disable

  export MANGOHUD=${MANGOHUD:-1}

  source /opt/gow/launch-comp.sh
  launcher /usr/games/steam ${STEAM_STARTUP_FLAGS}
else
  STEAM_STARTUP_FLAGS=${STEAM_STARTUP_FLAGS:-"-bigpicture"}
  # shellcheck disable=SC2086
  exec /usr/games/steam ${STEAM_STARTUP_FLAGS}
fi
