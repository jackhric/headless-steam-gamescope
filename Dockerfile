FROM steam-stream-builder:m1 AS builder

COPY server /src/server
COPY build/peglib /deps/peglib
COPY build/enet /deps/enet

RUN cmake -S /src/server -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SERVER=ON \
        -DSIMPLE_WEB_SERVER_DIR="$SIMPLE_WEB_SERVER_DIR" \
        -DPEGLIB_DIR=/deps/peglib -DENET_DIR=/deps/enet \
 && cmake --build /tmp/build -j"$(nproc)" \
        --target steam-stream-server gstrtpmoonlight

FROM local/steam-stream:dev

COPY --from=builder \
     /usr/lib/x86_64-linux-gnu/libboost_*.so.1.83.0 \
     /usr/lib/x86_64-linux-gnu/

RUN apt-get update -qq \
 && apt-get install -y --no-install-recommends pulseaudio pulseaudio-utils hwdata mangoapp \
 && rm -rf /var/lib/apt/lists/*

COPY --from=builder /tmp/build/steam-stream-server /usr/local/bin/steam-stream-server

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

RUN printf '/usr/local/nvidia/lib\n/usr/local/nvidia/lib64\n' \
      > /etc/ld.so.conf.d/00-steam-stream-nvidia.conf \
 && ldconfig

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
    # Vendored startup script (Deck-mode Steam flags + --xwayland-count 2) instead of the
    # base image's /opt/gow/startup-app.sh -- enables Steam's window switcher.
    STEAM_STREAM_LAUNCH_SCRIPT=/opt/steam-stream/startup-app.sh \
    # Add the DRI card + render nodes to GOW's device-group setup so `retro` can open them
    # (cont-init 15-setup_devices -> ensure-groups). Card-node access lets gamescope's XWayland
    # use glamor instead of falling back to software.
    GOW_REQUIRED_DEVICES="/dev/uinput /dev/input/event* /dev/dri/renderD128 /dev/dri/renderD129 /dev/dri/card0 /dev/dri/card1 /dev/dri/card2"

COPY rootfs/ /
RUN chmod +x /opt/steam-stream/entrypoint.sh /opt/steam-stream/pulse-sink.sh \
    /opt/steam-stream/startup-app.sh

ENTRYPOINT ["/opt/steam-stream/entrypoint.sh"]
