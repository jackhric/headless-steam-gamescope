#!/usr/bin/env bash
# Offline end-to-end verify of the final image on an isolated network (no host ports bound):
# boots the real entrypoint, drives REST->RTSP->PLAY, asserts the pipeline emits RTP and the
# launcher forks gamescope+Steam. A client cert is pre-seeded so /launch skips live pairing.
set -uo pipefail

IMAGE="${1:-local/steam-stream:latest}"
NAME="ss-m6-verify"
APPID="${APPID:-1}"
RENDER_NODE="${STEAM_STREAM_RENDER_NODE:-/dev/dri/renderD129}"
WORK="$(mktemp -d)"
trap 'docker rm -f "$NAME" >/dev/null 2>&1; rm -rf "$WORK"' EXIT

docker rm -f "$NAME" >/dev/null 2>&1 || true

# Seed the cert before the server loads paired clients at init.
mkdir -p "$WORK/state/clients"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$WORK/cli.key" -out "$WORK/cli.crt" \
  -days 3650 -subj "/CN=m6-verify-client" >/dev/null 2>&1
cp "$WORK/cli.crt" "$WORK/state/clients/m6.pem"
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

echo "-- /serverinfo over HTTP (unpaired) --"
SERVERINFO_OK=0
for _ in $(seq 1 20); do
  docker exec "$NAME" curl -s -m 3 http://127.0.0.1:47989/serverinfo >"$WORK/serverinfo.xml" 2>/dev/null
  grep -q "<root" "$WORK/serverinfo.xml" && { SERVERINFO_OK=1; break; }
  sleep 1
done
echo "serverinfo_ok=$SERVERINFO_OK ($(grep -o '<state>[^<]*</state>' "$WORK/serverinfo.xml" 2>/dev/null))"

echo "################ drive REST -> RTSP -> PLAY (appid=$APPID) inside the container ################"
docker cp "$WORK/cli.crt" "$NAME:/tmp/cli.crt"
docker cp "$WORK/cli.key" "$NAME:/tmp/cli.key"
docker exec -i "$NAME" python3 - "$APPID" <<'PY'
import http.client, ssl, sys, socket, re
APPID = sys.argv[1]
def ctx(crt,key):
    c=ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); c.check_hostname=False
    c.verify_mode=ssl.CERT_NONE; c.load_cert_chain(crt,key); return c
h=http.client.HTTPSConnection("127.0.0.1",47984,context=ctx("/tmp/cli.crt","/tmp/cli.key"),timeout=5)
h.request("GET","/launch?appid=%s&mode=1280x720x60&rikey=9d804e9d3a7d8f1b2c3d4e5f60718293&rikeyid=16909060&rtspport=48010"%APPID)
body=h.getresponse().read().decode()
m=re.search(r"rtsp://([0-9.]+):48010",body)
assert m, "no rtsp url in /launch response: %s" % body[:200]
HOST=m.group(1)
print("launch(appid=%s) -> rtsp://%s:48010"%(APPID,HOST))
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
RC=$?
echo "rtsp_driver_rc=$RC"

sleep 18

echo "################ container log (relevant) ################"
docker logs "$NAME" 2>&1 | grep -E \
  "cont-init|pulse sinks|null sink|steam-stream-server starting|PLAY for session|starting video pipeline|starting audio pipeline|compositor ready|\[LAUNCH\]|video appsink produced|audio appsink produced|no element|cudaconvertscale|Failed to|ERROR" \
  | grep -vE "appsink produced [0-9]+ RTP buffer\(s\) \(no client" | tail -50

echo "-- launched processes --"
docker exec "$NAME" bash -c 'pgrep -af "gamescope|sway --|/usr/games/steam|Xwayland" | grep -v steam-stream-server | head' 2>/dev/null || true

LOG="$(docker logs "$NAME" 2>&1)"
echo "################ assertions ################"
fail=0
chk(){ if grep -q "$1" <<<"$LOG"; then echo "PASS: $2"; else echo "FAIL: $2"; fail=1; fi; }
[ "$UP" = 1 ]          && echo "PASS: server came up"            || { echo "FAIL: server did not start"; fail=1; }
[ "$SERVERINFO_OK" = 1 ] && echo "PASS: /serverinfo responds"    || { echo "FAIL: /serverinfo no response"; fail=1; }
chk "pulse sinks:.*steam-stream"   "PulseAudio steam-stream null sink up"
chk "PLAY for session"             "RTSP PLAY hook fired"
chk "starting video pipeline"      "video pipeline started (cudaconvertscale registered)"
chk "compositor ready"             "gst-wayland-display compositor announced WAYLAND_DISPLAY"
chk "\[LAUNCH\]"                   "launcher fired (gamescope/sway)"
chk "video appsink produced"       "video RTP buffers emitted"
if grep -q "no element \"cudaconvertscale\"\|no element \"cudaupload\"" <<<"$LOG"; then
  echo "FAIL: video pipeline missing CUDA elements (libnvrtc/ld-path regression)"; fail=1
fi
[ $fail -eq 0 ] && echo "=== M6 IMAGE VERIFY PASSED (appid=$APPID) ===" \
                || echo "=== M6 IMAGE VERIFY FAILED (appid=$APPID) ==="
exit $fail
