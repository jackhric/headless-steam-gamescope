#!/bin/sh
# Override of GOW's system-services.sh: drops Decky Loader, which CEF-injects into
# steamwebhelper and crash-loops on this host, taking Big Picture down with it.

source /opt/gow/bash-lib/utils.sh

# Steam Big Picture first-time setup needs a session/system bus to talk to.
mkdir -p /run/dbus
dbus-daemon --system --fork --nosyslog
gow_log "*** DBus started ***"

# bluez + NetworkManager back the BPM Bluetooth/Network panels; harmless if they fail headless.
bluetoothd --nodetach &
gow_log "*** Bluez started ***"
NetworkManager &
gow_log "*** NetworkManager started ***"

# Translates the Steam power menu's login1 D-Bus call into `steam -shutdown`.
steamos-dbus-watchdog.sh &
gow_log "*** D-Bus Watchdog started ***"

# A prior boot may have left a Decky autostart on the /home volume; keep it dormant and
# clear the CEF remote-debugging marker so steamwebhelper runs clean.
if [ -n "${HOME:-}" ] && [ -x "${HOME}/homebrew/services/PluginLoader" ]; then
  gow_log "[system-services] Decky PluginLoader present on volume -- leaving it dormant (not started)"
fi
pkill -f 'homebrew/services/PluginLoader' 2>/dev/null || true
rm -f "${HOME:-/home/retro}/.steam/debian-installation/.cef-enable-remote-debugging" 2>/dev/null || true

disown 2>/dev/null || true
