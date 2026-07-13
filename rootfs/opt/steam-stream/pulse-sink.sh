#!/usr/bin/env bash
# (Re)create the steam-stream null sink with the requested channel layout: pulse-sink.sh <2|6|8> [sink_name].
# Idempotent; a sink's channel map is fixed at module load, so a layout change means unload + reload.
# Must run as the PulseAudio user (retro) with XDG_RUNTIME_DIR pointing at the pulse socket dir.
set -uo pipefail

CHANNELS="${1:-2}"
SINK="${2:-steam-stream}"

# Channel maps in the order Moonlight/Opus expect (Windows KSAUDIO speaker order).
case "${CHANNELS}" in
  2) MAP="front-left,front-right" ;;
  6) MAP="front-left,front-right,front-center,lfe,rear-left,rear-right" ;;
  8) MAP="front-left,front-right,front-center,lfe,rear-left,rear-right,side-left,side-right" ;;
  *)
    echo "pulse-sink: unsupported channel count '${CHANNELS}', using stereo" >&2
    CHANNELS=2
    MAP="front-left,front-right"
    ;;
esac

# `pactl list short sinks`: index name driver <format Nch rateHz> state
current="$(pactl list short sinks 2>/dev/null | awk -v s="${SINK}" '$2 == s {print $5}')"
if [ "${current}" = "${CHANNELS}ch" ]; then
  pactl set-default-sink "${SINK}" 2>/dev/null || true
  exit 0
fi

if [ -n "${current}" ]; then
  mod="$(pactl list short modules 2>/dev/null |
    awk -v re="sink_name=${SINK}([[:space:]]|$)" '$2 == "module-null-sink" && $0 ~ re {print $1; exit}')"
  if [ -n "${mod}" ]; then
    pactl unload-module "${mod}" || echo "pulse-sink: failed to unload module ${mod}" >&2
  fi
fi

pactl load-module module-null-sink "sink_name=${SINK}" rate=48000 format=float32le \
  "channels=${CHANNELS}" "channel_map=${MAP}" \
  "sink_properties=device.description=${SINK}" >/dev/null || {
  echo "pulse-sink: failed to load ${CHANNELS}ch null sink '${SINK}'" >&2
  exit 1
}
pactl set-default-sink "${SINK}" 2>/dev/null || true
echo "pulse-sink: sink '${SINK}' ready (${CHANNELS}ch, map ${MAP})"
