#!/usr/bin/env bash
# Build the steam-stream-server binary + unit tests. Deps are extracted from a local
# image (wolf-builder-check) rather than downloaded, so builds work offline.
set -euo pipefail

ROOT=/home/jackh/steam-stream
SRV="$ROOT/server"
OUT="$ROOT/build/server"
SWS="$ROOT/build/simple-web-server"
PEG="$ROOT/build/peglib"
ENET="$ROOT/build/enet"
IMAGE=steam-stream-builder:m1

mkdir -p "$OUT" "$PEG"

if [[ ! -f "$SWS/server_http.hpp" ]]; then
  echo "==> extracting Simple-Web-Server (546895a) from wolf-builder-check image"
  cid=$(docker create wolf-builder-check:latest)
  docker cp "$cid":/cache/cmake-build/_deps/simplewebserver-src "$SWS"
  docker rm "$cid" >/dev/null
fi

if [[ ! -f "$PEG/peglib.h" ]]; then
  echo "==> extracting cpp-peglib (peglib.h) from wolf-builder-check image"
  cid=$(docker create wolf-builder-check:latest)
  docker cp "$cid":/cache/cmake-build/_deps/peglib-src/peglib.h "$PEG/peglib.h"
  docker rm "$cid" >/dev/null
fi

if [[ ! -f "$ENET/include/enet/enet.h" ]]; then
  echo "==> extracting Moonlight ENet fork (enet-src) from wolf-builder-check image"
  cid=$(docker create wolf-builder-check:latest)
  docker cp "$cid":/cache/cmake-build/_deps/enet-src "$ENET"
  docker rm "$cid" >/dev/null
fi

echo "==> compiling steam-stream-server + tests (CMake, in $IMAGE)"
docker run --rm \
  -v "$SRV":/work -v "$SWS":/sws -v "$PEG":/peg -v "$ENET":/enet -v "$OUT":/out \
  "$IMAGE" -c '
    set -e
    cmake -S /work -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SERVER=ON -DSIMPLE_WEB_SERVER_DIR=/sws -DPEGLIB_DIR=/peg -DENET_DIR=/enet
    cmake --build /tmp/build -j"$(nproc)" \
      --target steam-stream-server test_pairing test_rtsp test_control \
               test_fec test_aes test_xml test_session_key test_uinput
    for b in steam-stream-server test_pairing test_rtsp test_control \
             test_fec test_aes test_xml test_session_key test_uinput; do
      cp -v "/tmp/build/$b" /out/
    done
  '

echo "==> done: $OUT/{steam-stream-server,test_pairing,test_rtsp,test_control,test_fec,test_aes,test_xml,test_session_key,test_uinput}"
echo "    (full structured suite: server/tests/run-tests.sh)"
