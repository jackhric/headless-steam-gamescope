# steam-stream — final single-container Moonlight host.
#
#   ONE container = the GOW Steam userspace (Steam + gamescope + sway + the /opt/gow launch
#   scripts) + Wolf's self-built GStreamer 1.26 / gst-wayland-display / CUDA plugins in
#   /usr/local + OUR OWN Moonlight host `steam-stream-server` (M1-M5, NOT the Wolf binary)
#   + the M1 `rtpmoonlightpay` GStreamer plugin.
#
# The server is its own pairing/REST/RTSP/ENet-control/RTP implementation; on PLAY it builds
# the waylanddisplaysrc -> nvenc -> rtpmoonlightpay_video pipeline and launches Steam (gamescope
# Big Picture / sway Desktop) as user `retro` into the session's gst-wayland-display compositor.
# Single shared /home/retro -> one Steam login/library, two tiles.
#
# Base + both grafted stacks are Ubuntu 25.04, so apt/lib versions line up.
FROM local/steam-stream:dev

# ---- Boost 1.83 runtime --------------------------------------------------------------------
# steam-stream-server + libgstrtpmoonlightpay.so were built on the 24.04 builder
# (steam-stream-builder:m1) and link Boost soname 1.83; the 25.04 base ships 1.88. Copy the
# 1.83 runtime libs in (matches server/tests/m5-verify.Dockerfile).
COPY --from=steam-stream-builder:m1 \
     /usr/lib/x86_64-linux-gnu/libboost_*.so.1.83.0 \
     /usr/lib/x86_64-linux-gnu/

# ---- PulseAudio ----------------------------------------------------------------------------
# The steam-emu base has no sound server; M5's audio path reads pulsesrc device=steam-stream.monitor.
# The entrypoint stands up PulseAudio + a module-null-sink sink_name=steam-stream as `retro`.
# hwdata ships /usr/share/hwdata/pci.ids; without it gamescope/Vulkan/libpciaccess log a
# "Failed to read pci.ids" warning on every launch (cosmetic, but noisy).
RUN apt-get update -qq \
 && apt-get install -y --no-install-recommends pulseaudio pulseaudio-utils hwdata \
 && rm -rf /var/lib/apt/lists/*

# ---- Our server binary + the M1 rtpmoonlightpay plugin -------------------------------------
COPY build/server/steam-stream-server /usr/local/bin/steam-stream-server
# Drop the payloader into Wolf's GStreamer plugin dir so it is on the default scan path
# alongside waylanddisplaysrc / cudaupload / nvh264enc.
COPY build/plugins/libgstrtpmoonlightpay.so \
     /usr/local/lib/x86_64-linux-gnu/gstreamer-1.0/

# ---- NVIDIA ld path (REQUIRED) -------------------------------------------------------------
# The NVIDIA Container Toolkit / CDI injects the driver userspace (incl. libnvrtc) under
# /usr/local/nvidia/lib at runtime, but the base nvidia.conf only lists /usr/nvidia/lib. Without
# /usr/local/nvidia/lib on the ld path, ldconfig never finds libnvrtc, so cudaconvert / cudascale
# / cudaconvertscale (which JIT their kernels via nvrtc) fail to register and the video pipeline
# cannot be constructed ("no element cudaconvertscale"). Add it; cont-init 30-nvidia.sh's ldconfig
# then picks up the CDI-injected libnvrtc.
RUN printf '/usr/local/nvidia/lib\n/usr/local/nvidia/lib64\n' \
      > /etc/ld.so.conf.d/00-steam-stream-nvidia.conf \
 && ldconfig

# ---- runtime env ---------------------------------------------------------------------------
ENV GST_PLUGIN_PATH=/usr/local/lib/x86_64-linux-gnu/gstreamer-1.0 \
    # Non-zero-copy RGBx capture path; the CUDAMemory zero-copy path panics under CDI.
    WOLF_USE_ZERO_COPY=FALSE \
    WOLF_RENDER_NODE=/dev/dri/renderD129 \
    STEAM_STREAM_RENDER_NODE=/dev/dri/renderD129 \
    LD_LIBRARY_PATH=/usr/local/nvidia/lib:/usr/local/nvidia/lib64 \
    # Persist pairing/cert/session state on the /home/retro volume so pairings survive restarts.
    STEAM_STREAM_STATE_DIR=/home/retro/.steam-stream \
    # Add the DRI card + render nodes to GOW's device-group setup so `retro` can open them
    # (cont-init 15-setup_devices -> ensure-groups). Card-node access lets gamescope's XWayland
    # use glamor instead of falling back to software.
    GOW_REQUIRED_DEVICES="/dev/uinput /dev/input/event* /dev/dri/renderD128 /dev/dri/renderD129 /dev/dri/card0 /dev/dri/card1 /dev/dri/card2"

# ---- mangoapp no-op stub -------------------------------------------------------------------
# GOW's startup-app.sh unconditionally backgrounds `mangoapp` (the gamescope stats overlay),
# which the Ubuntu base-app doesn't ship -> "mangoapp: command not found" noise on every launch.
# It is purely cosmetic (no overlay), so provide a silent no-op on PATH to keep logs clean.
RUN printf '#!/bin/sh\nexit 0\n' > /usr/local/bin/mangoapp \
 && chmod +x /usr/local/bin/mangoapp

# ---- entrypoint ----------------------------------------------------------------------------
COPY rootfs/ /
RUN chmod +x /opt/steam-stream/entrypoint.sh

ENTRYPOINT ["/opt/steam-stream/entrypoint.sh"]
