#!/bin/sh

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

if [ -n "${HOME:-}" ] && [ -x "${HOME}/homebrew/services/PluginLoader" ]; then
  gow_log "[system-services] Decky PluginLoader present on volume -- leaving it dormant (not started)"
fi
pkill -f 'homebrew/services/PluginLoader' 2>/dev/null || true
rm -f "${HOME:-/home/retro}/.steam/debian-installation/.cef-enable-remote-debugging" 2>/dev/null || true

disown 2>/dev/null || true
