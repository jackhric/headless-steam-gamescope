#!/usr/bin/env bash
# steam-stream test suite: unit tier (always, CPU-only) + integration tier (needs GPU +
# /dev/uinput, runs on container-internal ports only). Offline; SKIP_INTEGRATION=1 for unit only.
set -uo pipefail

ROOT=/home/jackh/steam-stream
SRV="$ROOT/server"
OUT="$ROOT/build/server"
BUILD="$ROOT/build/tests-build"   # shared by both ctest runs
SWS="$ROOT/build/simple-web-server"
PEG="$ROOT/build/peglib"
ENET="$ROOT/build/enet"
PLUGINS="$ROOT/build/plugins"
IMAGE=steam-stream-builder:m1
RESULTS="$BUILD/results"

mkdir -p "$OUT" "$PEG" "$BUILD" "$RESULTS"
rm -f "$RESULTS"/*

UNIT_TARGETS="test_pairing test_rtsp test_control test_resume_key test_fec test_aes test_xml test_session_key"
INTEG_BIN_TARGETS="test_uinput"

# Offline dependency extraction (same no-clone pattern as build-server.sh).
extract_dep() { # <check-file> <src-in-image> <dest>
  if [[ ! -e "$1" ]]; then
    echo "==> extracting $(basename "$3") from wolf-builder-check image"
    cid=$(docker create wolf-builder-check:latest)
    docker cp "$cid":"$2" "$3"
    docker rm "$cid" >/dev/null
  fi
}
extract_dep "$SWS/server_http.hpp" /cache/cmake-build/_deps/simplewebserver-src "$SWS"
extract_dep "$PEG/peglib.h"        /cache/cmake-build/_deps/peglib-src/peglib.h "$PEG/peglib.h"
extract_dep "$ENET/include/enet/enet.h" /cache/cmake-build/_deps/enet-src "$ENET"

# GPU / uinput detection gates the integration tier.
GPU=0
if [[ "${SKIP_INTEGRATION:-0}" != "1" ]]; then
  if command -v nvidia-smi >/dev/null 2>&1 && nvidia-smi -L 2>/dev/null | grep -q GPU \
       && [[ -e /dev/uinput ]] && [[ -e "$PLUGINS/libgstrtpmoonlightpay.so" ]]; then
    GPU=1
  fi
fi
RENDER_NODE="${STEAM_STREAM_RENDER_NODE:-/dev/dri/renderD129}"

echo "######################## BUILD + UNIT TESTS ########################"
docker run --rm \
  -v "$SRV":/work -v "$SWS":/sws -v "$PEG":/peg -v "$ENET":/enet \
  -v "$BUILD":/build -v "$OUT":/out -v "$RESULTS":/results \
  "$IMAGE" -c "
    set -e
    cmake -S /work -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SERVER=ON -DSIMPLE_WEB_SERVER_DIR=/sws -DPEGLIB_DIR=/peg -DENET_DIR=/enet >/dev/null
    cmake --build /build -j\"\$(nproc)\" \
      --target steam-stream-server $UNIT_TARGETS $INTEG_BIN_TARGETS
    cp -f /build/steam-stream-server /out/
    cp -f /work/tests/integration/verify-endpoints.sh /work/tests/integration/verify-media.sh /work/tests/integration/verify-m9-resume.sh /out/
    cd /build
    ctest -L unit --output-on-failure 2>&1 | tee /results/unit.log
    echo \"UNIT_RC=\${PIPESTATUS[0]}\" > /results/unit.rc
  "
UNIT_DOCKER_RC=$?

INTEG_RAN=0
if [[ "$GPU" == "1" ]]; then
  INTEG_RAN=1
  echo
  echo "######################## INTEGRATION TESTS (GPU) ########################"
  docker run --rm \
    --device nvidia.com/gpu=all -e NVIDIA_DRIVER_CAPABILITIES=all \
    --device /dev/uinput \
    -e GST_PLUGIN_PATH=/plugins \
    -e STEAM_STREAM_RENDER_NODE="$RENDER_NODE" \
    -e SERVER_BIN=/out/steam-stream-server \
    -v "$BUILD":/build -v "$OUT":/out -v "$PLUGINS":/plugins -v "$RESULTS":/results \
    "$IMAGE" -c "
      cd /build
      ctest -L integration --output-on-failure; echo \"UINPUT_RC=\$?\" > /results/uinput.rc

      echo; echo '==== integration: REST -> RTSP -> PLAY endpoint flow ===='
      bash /out/verify-endpoints.sh; echo \"ENDPOINTS_RC=\$?\" > /results/endpoints.rc

      echo; echo '==== integration: media pipeline (nvenc -> rtpmoonlightpay -> RTP) ===='
      bash /out/verify-media.sh; echo \"MEDIA_RC=\$?\" > /results/media.rc

      echo; echo '==== integration: resume/reconnect (reuse app, re-target RTP, force IDR) ===='
      bash /out/verify-m9-resume.sh; echo \"RESUME_RC=\$?\" > /results/resume.rc
    "
else
  echo
  echo "######################## INTEGRATION TESTS: SKIPPED ########################"
  echo "(no GPU/uinput/plugin detected, or SKIP_INTEGRATION=1) -- unit tier only."
fi

# Tally + summary.
get_rc() { [[ -f "$RESULTS/$1" ]] && sed -E 's/.*=//' "$RESULTS/$1" || echo 99; }

echo
echo "============================== SUMMARY =============================="

PASS=0; TOTAL=0
declare -a LINES

if [[ -f "$RESULTS/unit.log" ]]; then
  while IFS= read -r line; do
    name=$(sed -E 's/.*Test #[0-9]+: ([A-Za-z0-9_]+) .*/\1/' <<<"$line")
    TOTAL=$((TOTAL+1))
    if grep -q "Passed" <<<"$line"; then PASS=$((PASS+1)); LINES+=("  PASS [unit]        $name");
    else LINES+=("  FAIL [unit]        $name"); fi
  done < <(grep -E 'Test +#[0-9]+:' "$RESULTS/unit.log")
fi

if [[ "$INTEG_RAN" == "1" ]]; then
  add() { # <rc> <label>
    TOTAL=$((TOTAL+1))
    if [[ "$1" == "0" ]]; then PASS=$((PASS+1)); LINES+=("  PASS [integration] $2");
    else LINES+=("  FAIL [integration] $2 (rc=$1)"); fi
  }
  add "$(get_rc uinput.rc)"    "test_uinput (virtual mouse/keyboard/gamepad creation)"
  add "$(get_rc endpoints.rc)" "verify-endpoints (serverinfo/pair/applist/launch + live RTSP)"
  add "$(get_rc media.rc)"     "verify-media (nvenc pipeline reaches PLAYING + emits RTP)"
  add "$(get_rc resume.rc)"    "verify-m9-resume (M9 reuse/re-target/IDR + M10 AES key rotation)"
else
  LINES+=("  SKIP [integration] test_uinput / verify-endpoints / verify-media (no GPU)")
fi

printf '%s\n' "${LINES[@]}"
echo "--------------------------------------------------------------------"
echo "PASS/FAIL: $PASS/$TOTAL"
[[ "$INTEG_RAN" == "0" ]] && echo "(integration tier skipped -- run on the GPU box for full coverage)"

if [[ "$UNIT_DOCKER_RC" == "0" && "$PASS" == "$TOTAL" ]]; then
  echo "RESULT: GREEN"; exit 0
else
  echo "RESULT: RED"; exit 1
fi
