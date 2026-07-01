# M5 verification image: the merged steam-stream userspace (Steam + gamescope + sway +
# GOW launch scripts + Wolf's /usr/local GStreamer + gst-wayland-display) plus the runtime
# bits M5/M6 need that the steam-emu base lacks:
#   * PulseAudio (for the steam-stream null sink / .monitor audio path),
#   * the Boost 1.83 runtime the server + rtpmoonlightpay plugin were built against on the
#     24.04 builder (the 25.04 base ships Boost 1.88 -- soname mismatch),
#   * a `retro` user at uid 1000 (the steam-emu base only has `ubuntu` at 1000; the GOW
#     cont-init normally creates retro at runtime -- M6 wires that in the entrypoint).
# The server binary and the M1 rtpmoonlightpay plugin are mounted at runtime, not baked.
FROM local/steam-stream:dev

RUN apt-get update -qq \
 && apt-get install -y --no-install-recommends pulseaudio pulseaudio-utils \
 && rm -rf /var/lib/apt/lists/*

# Boost 1.83 runtime (server + plugin link these; base 25.04 has the wrong soname).
COPY --from=steam-stream-builder:m1 \
     /usr/lib/x86_64-linux-gnu/libboost_*.so.1.83.0 \
     /usr/lib/x86_64-linux-gnu/

RUN ldconfig \
 && usermod  -l retro -d /home/retro ubuntu \
 && groupmod -n retro ubuntu \
 && chown -R retro:retro /home/retro
