#!/usr/bin/env bash
# Offline verify of two fixes: BUG 1 (input) — with STEAM_STREAM_INPUT_SELFTEST=1, synthetic
# events into waylanddisplaysrc must return accepted=yes (reached the compositor seat). BUG 2
# (render) — the NVIDIA GBM backend symlink gives Xwayland hardware EGL, not swrast fallback.
set -uo pipefail

IMAGE="${1:-local/steam-stream:latest}"
NAME="ss-m7-verify"
RENDER_NODE="${STEAM_STREAM_RENDER_NODE:-/dev/dri/renderD129}"
WORK="$(mktemp -d)"
trap 'docker rm -f "$NAME" >/dev/null 2>&1; rm -rf "$WORK"' EXIT

docker rm -f "$NAME" >/dev/null 2>&1 || true

mkdir -p "$WORK/state/clients"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$WORK/cli.key" -out "$WORK/cli.crt" \
  -days 3650 -subj "/CN=m7-verify-client" >/dev/null 2>&1
cp "$WORK/cli.crt" "$WORK/state/clients/m7.pem"
chmod -R 777 "$WORK/state"

echo "################ boot container (real entrypoint; --network none) ################"
docker run -d --rm --name "$NAME" \
  --network none \
  --device nvidia.com/gpu=all -e NVIDIA_DRIVER_CAPABILITIES=all \
  --device /dev/uinput --device /dev/dri \
  --group-add 989 --group-add 985 --group-add 994 \
  --shm-size 2g \
  -e STEAM_STREAM_RENDER_NODE="$RENDER_NODE" \
  -e STEAM_STREAM_STATE_DIR=/seed-state \
  -e STEAM_STREAM_INPUT_SELFTEST=1 \
  -e RUST_LOG=info \
  -v "$WORK/state:/seed-state" \
  "$IMAGE" >/dev/null

echo "-- waiting for steam-stream-server to come up --"
UP=0
for _ in $(seq 1 40); do
  if docker logs "$NAME" 2>&1 | grep -q "steam-stream-server starting"; then UP=1; break; fi
  sleep 1
done
echo "server_up=$UP"

echo "################ BUG 2: NVIDIA GBM backend (entrypoint fix) ################"
GBM_LINK="$(docker exec "$NAME" readlink -f /usr/lib/x86_64-linux-gnu/gbm/nvidia-drm_gbm.so 2>/dev/null)"
echo "nvidia-drm_gbm.so -> ${GBM_LINK:-<missing>}"
GBM_EGL_VENDOR="$(docker exec "$NAME" bash -c 'eglinfo 2>/dev/null | awk "/GBM platform/{f=1} f&&/EGL vendor string/{print \$NF; exit}"')"
echo "GBM-platform EGL vendor: ${GBM_EGL_VENDOR:-<none>}"

echo "################ drive REST -> RTSP -> PLAY (appid=1) inside the container ################"
docker cp "$WORK/cli.crt" "$NAME:/tmp/cli.crt"
docker cp "$WORK/cli.key" "$NAME:/tmp/cli.key"
docker exec -i "$NAME" python3 - <<'PY'
import http.client, ssl, socket, re
def ctx(crt,key):
    c=ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); c.check_hostname=False
    c.verify_mode=ssl.CERT_NONE; c.load_cert_chain(crt,key); return c
h=http.client.HTTPSConnection("127.0.0.1",47984,context=ctx("/tmp/cli.crt","/tmp/cli.key"),timeout=5)
h.request("GET","/launch?appid=1&mode=1280x720x60&rikey=9d804e9d3a7d8f1b2c3d4e5f60718293&rikeyid=16909060&rtspport=48010")
body=h.getresponse().read().decode()
m=re.search(r"rtsp://([0-9.]+):48010",body); assert m, body[:200]
HOST=m.group(1); print("launch -> rtsp://%s:48010"%HOST)
def rtsp(msg):
    s=socket.create_connection(("127.0.0.1",48010),timeout=5)
    s.sendall(msg.encode()); s.shutdown(socket.SHUT_WR)
    d=b""
    while True:
        c=s.recv(4096)
        if not c: break
        d+=c
    s.close(); return d.decode(errors="replace")
assert "200 OK" in rtsp("OPTIONS rtsp://%s:48010 RTSP/1.0\r\nCSeq:1\r\nHost:%s\r\n\r\n"%(HOST,HOST))
rtsp("DESCRIBE rtsp://%s:48010 RTSP/1.0\r\nCSeq:2\r\nHost:%s\r\nAccept: application/sdp\r\n\r\n"%(HOST,HOST))
for sid in ("video","audio","control"):
    rtsp("SETUP streamid=%s/0/0 RTSP/1.0\r\nCSeq:3\r\nHost:%s\r\n\r\n"%(sid,HOST))
sdp=("v=0\na=x-nv-video[0].clientViewportWd:1280 \na=x-nv-video[0].clientViewportHt:720 \n"
     "a=x-nv-video[0].maxFPS:60 \na=x-nv-video[0].packetSize:1024 \n"
     "a=x-nv-vqos[0].bw.maximumBitrateKbps:15500 \na=x-nv-vqos[0].fec.minRequiredFecPackets:2 \n"
     "a=x-nv-general.featureFlags:167 \na=x-nv-audio.surround.numChannels:2 \n"
     "a=x-nv-aqos.packetDuration:5 \na=x-nv-video[0].encoderCscMode:0 \na=x-nv-vqos[0].bitStreamFormat:0 \n")
assert "200 OK" in rtsp("ANNOUNCE streamid=control/13/0 RTSP/1.0\r\nCSeq:6\r\nHost:%s\r\n"
                        "Content-type: application/sdp\r\nContent-length:%d\r\n\r\n%s"%(HOST,len(sdp),sdp))
assert "200 OK" in rtsp("PLAY rtsp://%s:48010 RTSP/1.0\r\nCSeq:7\r\nHost:%s\r\n\r\n"%(HOST,HOST))
print("RTSP OPTIONS..PLAY -> 200 OK")
PY
echo "rtsp_driver_rc=$?"

sleep 25

LOG="$(docker logs "$NAME" 2>&1)"

echo "################ input self-test + gamescope log (relevant) ################"
grep -E "INPUT-SELFTEST|compositor ready|\[LAUNCH\]|glamor|EGL|falling back to sw|disabling glamor" <<<"$LOG" | tail -40

echo "################ assertions ################"
fail=0
chk(){ if grep -qE "$1" <<<"$LOG"; then echo "PASS: $2"; else echo "FAIL: $2"; fail=1; fi; }

[ "$UP" = 1 ] && echo "PASS: server came up" || { echo "FAIL: server did not start"; fail=1; }

# BUG 2
[ -n "$GBM_LINK" ] && echo "PASS: nvidia-drm_gbm.so symlink present" \
                   || { echo "FAIL: nvidia-drm_gbm.so symlink missing"; fail=1; }
[ "$GBM_EGL_VENDOR" = "NVIDIA" ] && echo "PASS: GBM-platform EGL = NVIDIA (hardware glamor)" \
   || { echo "FAIL: GBM-platform EGL = '$GBM_EGL_VENDOR' (expected NVIDIA, not Mesa/swrast)"; fail=1; }
if grep -qE "falling back to sw|EGL setup failed, disabling glamor" <<<"$LOG"; then
  echo "FAIL: gamescope Xwayland fell back to software glamor"; fail=1
else
  echo "PASS: no Xwayland software-glamor fallback in log"
fi

# BUG 1
chk "INPUT-SELFTEST. MouseMoveAbsolute accepted=yes" "compositor accepted MouseMoveAbsolute (touch/abs)"
chk "INPUT-SELFTEST. KeyboardKey accepted=yes"        "compositor accepted KeyboardKey"
chk "INPUT-SELFTEST. MouseButton accepted=yes"        "compositor accepted MouseButton"

[ $fail -eq 0 ] && echo "=== M7 FIXES VERIFY PASSED ===" || echo "=== M7 FIXES VERIFY FAILED ==="
exit $fail
