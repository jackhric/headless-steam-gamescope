#!/usr/bin/env bash
# Build the standalone rtpmoonlightpay plugin .so inside the Wolf-based builder image
# and drop the artifact in /home/jackh/steam-stream/build/plugins/.
set -euo pipefail

SRV=/home/jackh/steam-stream/server
OUT=/home/jackh/steam-stream/build/plugins
IMAGE=steam-stream-builder:m1

mkdir -p "$OUT"

echo "==> building builder image ($IMAGE)"
docker build -t "$IMAGE" "$SRV"

echo "==> compiling plugin"
docker run --rm \
    -v "$SRV":/work \
    -v "$OUT":/out \
    "$IMAGE" -c '
        set -e
        cmake -S /work -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=Release
        cmake --build /tmp/build -j"$(nproc)"
        cp -v /tmp/build/libgstrtpmoonlightpay.so /out/
    '

echo "==> done: $OUT/libgstrtpmoonlightpay.so"
