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
# ============================================================================================
# STAGE 1 (builder): compile steam-stream-server + the rtpmoonlightpay plugin.
#
# The builder is Ubuntu 24.04 (Wolf's toolchain image) so we link against Wolf's exact
# GStreamer 1.26 ABI; the runtime base below is 25.04. That distro mismatch is why the final
# stage copies the Boost 1.83 runtime libs out of here. Simple-Web-Server is pre-cloned in the
# builder image (server/Dockerfile); peglib.h + enet-src are vendored in the repo build/ dir
# (extracted offline by server/build-server.sh) and copied in from the build context.
# ============================================================================================
FROM steam-stream-builder:m1 AS builder

COPY server /src/server
COPY build/peglib /deps/peglib
COPY build/enet /deps/enet

# SIMPLE_WEB_SERVER_DIR is set in the builder image ENV (/opt/Simple-Web-Server).
RUN cmake -S /src/server -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SERVER=ON \
        -DSIMPLE_WEB_SERVER_DIR="$SIMPLE_WEB_SERVER_DIR" \
        -DPEGLIB_DIR=/deps/peglib -DENET_DIR=/deps/enet \
 && cmake --build /tmp/build -j"$(nproc)" \
        --target steam-stream-server gstrtpmoonlight

# ============================================================================================
# STAGE 2 (runtime): the final single-container Moonlight host.
# Base + both grafted stacks are Ubuntu 25.04, so apt/lib versions line up.
# ============================================================================================
FROM local/steam-stream:dev

# ---- Boost 1.83 runtime --------------------------------------------------------------------
# steam-stream-server + libgstrtpmoonlightpay.so are built on the 24.04 builder stage and link
# Boost soname 1.83; the 25.04 base ships 1.88. Copy the 1.83 runtime libs in.
COPY --from=builder \
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
COPY --from=builder /tmp/build/steam-stream-server /usr/local/bin/steam-stream-server
# Drop the payloader into Wolf's GStreamer plugin dir so it is on the default scan path
# alongside waylanddisplaysrc / cudaupload / nvh264enc.
COPY --from=builder /tmp/build/libgstrtpmoonlightpay.so \
     /usr/local/lib/x86_64-linux-gnu/gstreamer-1.0/

# ---- prune unused GStreamer plugins with unmet lib deps ------------------------------------
# These optional leaf plugins (barcode/QR/HDR-image + the legacy vaapi plugin; we use the new
# `va` plugin, libgstva.so, instead) are missing their runtime libs, so GStreamer logs a
# blacklist WARNING for each on every plugin scan. None are in our capture/encode pipeline, so
# remove them to keep the registry scan clean.
RUN rm -f \
      /usr/local/lib/x86_64-linux-gnu/gstreamer-1.0/libgstopenexr.so \
      /usr/local/lib/x86_64-linux-gnu/gstreamer-1.0/libgstzbar.so \
      /usr/local/lib/x86_64-linux-gnu/gstreamer-1.0/libgstzxing.so \
      /usr/local/lib/x86_64-linux-gnu/gstreamer-1.0/libgstvaapi.so

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
    # True zero-copy: waylanddisplaysrc emits memory:CUDAMemory directly (verified working under
    # CDI on this box). Set FALSE to fall back to the RGBx + cudaupload path.
    WOLF_USE_ZERO_COPY=TRUE \
    WOLF_RENDER_NODE=/dev/dri/renderD129 \
    STEAM_STREAM_RENDER_NODE=/dev/dri/renderD129 \
    LD_LIBRARY_PATH=/usr/local/nvidia/lib:/usr/local/nvidia/lib64 \
    # Persist pairing/cert/session state on the /home/retro volume so pairings survive restarts.
    STEAM_STREAM_STATE_DIR=/home/retro/.steam-stream \
    # Encoder config on a dedicated /config mount (bind a host dir here) -- kept OUT of the
    # container/state volume so it's editable on the host and by a future web UI.
    STEAM_STREAM_ENCODER_CONFIG=/config/encoders.toml \
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
