# Build + run environment for the standalone rtpmoonlightpay GStreamer plugin.
#
# Based on the Wolf runtime image so we compile against the exact GStreamer 1.26.x
# in /usr/local (ABI must match the runtime). We only add a C/C++ toolchain and the
# handful of -dev packages the payloaders need (OpenSSL, Boost.Log, fmt, glib).
FROM ghcr.io/games-on-whales/wolf:stable

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        pkg-config \
        git \
        ca-certificates \
        libssl-dev \
        libfmt-dev \
        libglib2.0-dev \
        libboost-dev \
        libboost-log-dev \
        libboost-thread-dev \
        libboost-system-dev \
        libtomlplusplus-dev \
    && rm -rf /var/lib/apt/lists/*

# Header-only HTTP/HTTPS server lib, the exact fork+commit Wolf's rest/ uses
# (ReenigneArcher fork of eidheim/Simple-Web-Server, patched for recent Boost).
RUN git clone https://gitlab.com/ReenigneArcher/Simple-Web-Server.git /opt/Simple-Web-Server \
    && git -C /opt/Simple-Web-Server checkout 546895a9
ENV SIMPLE_WEB_SERVER_DIR=/opt/Simple-Web-Server

# Make pkg-config + the loader see Wolf's /usr/local GStreamer first.
ENV PKG_CONFIG_PATH=/usr/local/lib/x86_64-linux-gnu/pkgconfig:/usr/local/lib/pkgconfig
ENV LD_LIBRARY_PATH=/usr/local/lib/x86_64-linux-gnu:/usr/local/lib

# Override Wolf's supervisor entrypoint so the container is a plain shell for
# building / gst-launch testing.
ENTRYPOINT ["/bin/bash"]
