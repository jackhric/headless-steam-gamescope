#!/usr/bin/env bash
# Launch self-test: bring up PulseAudio, drive REST->RTSP->PLAY to launch the real app into
# the session compositor, and assert it connects + the media pipeline keeps emitting RTP.
set -uo pipefail

APPID="${1:-1}"                 # 1 = Big Picture (gamescope), 2 = Desktop (sway)
BIN=/out/steam-stream-server
RENDER_NODE="${STEAM_STREAM_RENDER_NODE:-/dev/dri/renderD129}"
export GST_PLUGIN_PATH="/plugins:${GST_PLUGIN_PATH:-/usr/local/lib/x86_64-linux-gnu/gstreamer-1.0}"
# nvrtc-JIT CUDA elements (cudaconvertscale) only register with libnvrtc on the lib path.
export LD_LIBRARY_PATH="/usr/local/nvidia/lib:/usr/local/nvidia/lib64:${LD_LIBRARY_PATH:-}"
export XDG_RUNTIME_DIR=/tmp/sockets
PULSE_SOCK="unix:${XDG_RUNTIME_DIR}/pulse/native"
ST=/tmp/ss-m5
rm -rf "$ST"; mkdir -p "$ST/clients" "$XDG_RUNTIME_DIR/pulse"
chmod 700 "$XDG_RUNTIME_DIR"
chown -R retro:retro "$XDG_RUNTIME_DIR" /home/retro 2>/dev/null || true

echo "################ 1. PulseAudio null sink ################"
runuser -u retro -- env XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR \
  pulseaudio --start --exit-idle-time=-1 --log-target=stderr 2>"$ST/pulse.log"
sleep 2
runuser -u retro -- env XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR \
  pactl load-module module-null-sink sink_name=steam-stream \
    sink_properties=device.description=steam-stream >/dev/null 2>>"$ST/pulse.log"
echo "-- sinks --"
runuser -u retro -- env XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR pactl list short sinks
SINK_OK=0
runuser -u retro -- env XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR pactl list short sinks | grep -q steam-stream && SINK_OK=1

echo "-- pulsesrc opens steam-stream.monitor --"
PULSESRC_OK=0
if runuser -u retro -- env XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR GST_PLUGIN_PATH="$GST_PLUGIN_PATH" \
     timeout 15 gst-launch-1.0 -q \
       pulsesrc device=steam-stream.monitor server="$PULSE_SOCK" num-buffers=20 ! \
       audioconvert ! fakesink >>"$ST/pulse.log" 2>&1; then
  PULSESRC_OK=1
fi
echo "sink_present=$SINK_OK pulsesrc_open=$PULSESRC_OK"

echo "################ 2. launch app via REST->RTSP->PLAY (appid=$APPID) ################"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$ST/cli.key" -out "$ST/cli.crt" \
  -days 3650 -subj "/CN=test-client" >/dev/null 2>&1
cp "$ST/cli.crt" "$ST/clients/manual.pem"

export STEAM_STREAM_STATE_DIR="$ST" STEAM_STREAM_LOG_LEVEL=INFO
export STEAM_STREAM_RENDER_NODE="$RENDER_NODE"
export PULSE_SERVER="$PULSE_SOCK" PULSE_SINK=steam-stream
export RUST_LOG="${RUST_LOG:-info}"
"$BIN" >"$ST/server.log" 2>&1 &
SRV=$!
sleep 2

python3 - "$ST" "$APPID" <<'PY'
import http.client, ssl, sys, socket, time, re
ST, APPID = sys.argv[1], sys.argv[2]
def ctx(crt,key):
    c=ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); c.check_hostname=False
    c.verify_mode=ssl.CERT_NONE; c.load_cert_chain(crt,key); return c
h=http.client.HTTPSConnection("127.0.0.1",47984,context=ctx(ST+"/cli.crt",ST+"/cli.key"),timeout=5)
h.request("GET","/launch?appid=%s&mode=1280x720x60&rikey=9d804e9d3a7d8f1b2c3d4e5f60718293&rikeyid=16909060&rtspport=48010"%APPID)
body=h.getresponse().read().decode()
FAKE=re.search(r"rtsp://([0-9.]+):48010",body).group(1)
print("launch(appid=%s) -> rtsp://%s:48010"%(APPID,FAKE))
def rtsp(m):
    s=socket.create_connection(("127.0.0.1",48010),timeout=5)
    s.sendall(m.encode()); s.shutdown(socket.SHUT_WR)
    d=b""
    while True:
        c=s.recv(4096)
        if not c: break
        d+=c
    s.close(); return d.decode(errors="replace")
assert "200 OK" in rtsp("OPTIONS rtsp://%s:48010 RTSP/1.0\r\nCSeq:1\r\nHost:%s\r\n\r\n"%(FAKE,FAKE))
rtsp("DESCRIBE rtsp://%s:48010 RTSP/1.0\r\nCSeq:2\r\nHost:%s\r\nAccept: application/sdp\r\n\r\n"%(FAKE,FAKE))
for sid in ("video","audio","control"):
    rtsp("SETUP streamid=%s/0/0 RTSP/1.0\r\nCSeq:3\r\nHost:%s\r\n\r\n"%(sid,FAKE))
sdp=("v=0\na=x-nv-video[0].clientViewportWd:1280 \na=x-nv-video[0].clientViewportHt:720 \n"
     "a=x-nv-video[0].maxFPS:60 \na=x-nv-video[0].packetSize:1024 \n"
     "a=x-nv-vqos[0].bw.maximumBitrateKbps:15500 \na=x-nv-vqos[0].fec.minRequiredFecPackets:2 \n"
     "a=x-nv-general.featureFlags:167 \na=x-nv-audio.surround.numChannels:2 \n"
     "a=x-nv-aqos.packetDuration:5 \na=x-nv-video[0].encoderCscMode:0 \na=x-nv-vqos[0].bitStreamFormat:0 \n")
assert "200 OK" in rtsp("ANNOUNCE streamid=control/13/0 RTSP/1.0\r\nCSeq:6\r\nHost:%s\r\n"
                        "Content-type: application/sdp\r\nContent-length:%d\r\n\r\n%s"%(FAKE,len(sdp),sdp))
assert "200 OK" in rtsp("PLAY rtsp://%s:48010 RTSP/1.0\r\nCSeq:7\r\nHost:%s\r\n\r\n"%(FAKE,FAKE))
print("RTSP OPTIONS..PLAY -> 200 OK; waiting for launch + frames...")
PY

PROC_RE='gamescope|sway --|/usr/games/steam|steamwebhelper|Xwayland'
for i in $(seq 1 40); do
  pgrep -af "$PROC_RE" | grep -qv steam-stream-server && break
  sleep 1
done
sleep 12

echo "-- launched processes (excluding our server) --"
pgrep -af "$PROC_RE" | grep -v steam-stream-server | head -20 || true
PROC_OK=0
pgrep -af "$PROC_RE" | grep -qv steam-stream-server && PROC_OK=1

# Check connection while the app is still alive (it dies with the compositor/server).
CONNECT_OK=0
if [ "$APPID" = 2 ]; then
  echo "-- swaymsg get_outputs --"
  OUTS=$(runuser -u retro -- env XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR \
           SWAYSOCK=$XDG_RUNTIME_DIR/sway.socket swaymsg -t get_outputs 2>/dev/null)
  echo "$OUTS" | grep -E '"name"|"width"|"active"' | head
  echo "$OUTS" | grep -q '"width"' && CONNECT_OK=1
else
  grep -qE "selecting physical device|Running compositor on wayland display|Starting Xwayland" \
    "$ST/server.log" && CONNECT_OK=1
fi
grep -q "Failed to connect to wayland socket" "$ST/server.log" && CONNECT_OK=0
grep -q "gbm_bo_create failed" "$ST/server.log" && echo "NOTE: wlroots GBM alloc failed (NVIDIA) -- WLR_RENDERER=pixman fallback should avoid this"
grep -q 'Created new socket.*wayland' "$ST/server.log" && echo "compositor socket: wayland-1 created"

kill $SRV 2>/dev/null
runuser -u retro -- env XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR pulseaudio --kill 2>/dev/null || true
sleep 1

echo "################ relevant server log ################"
grep -E "PLAY for session|starting (video|audio) pipeline|compositor ready|\[LAUNCH\]|selecting physical device|Running compositor on wayland|Starting Xwayland|Created new socket|Failed to connect to wayland|ERROR" \
  "$ST/server.log" | grep -vE "appsink produced" | head -40

echo "################ assertions ################"
log=$(cat "$ST/server.log")
fail=0
[ "$SINK_OK" = 1 ]     || { echo "FAIL: steam-stream null sink missing"; fail=1; }
[ "$PULSESRC_OK" = 1 ] || { echo "FAIL: pulsesrc could not open steam-stream.monitor"; fail=1; }
grep -q "PLAY for session"        <<<"$log" || { echo "FAIL: PLAY hook not hit"; fail=1; }
grep -q "starting video pipeline" <<<"$log" || { echo "FAIL: video pipeline not started"; fail=1; }
grep -q "compositor ready"        <<<"$log" || { echo "FAIL: compositor WAYLAND_DISPLAY not announced"; fail=1; }
grep -q "\[LAUNCH\]"              <<<"$log" || { echo "FAIL: launcher did not fire"; fail=1; }
grep -q "video appsink produced"  <<<"$log" || { echo "FAIL: no video RTP buffers"; fail=1; }
grep -q "audio appsink produced"  <<<"$log" || { echo "FAIL: no audio RTP buffers"; fail=1; }
[ "$PROC_OK" = 1 ]     || { echo "FAIL: no gamescope/steam/sway process running"; fail=1; }
[ "$CONNECT_OK" = 1 ]  || { echo "FAIL: launched app did not connect to the session compositor"; fail=1; }
[ $fail -eq 0 ] && echo "=== M5 LAUNCH SELF-TEST PASSED (appid=$APPID) ===" \
                || echo "=== M5 LAUNCH SELF-TEST FAILED (appid=$APPID) ==="
exit $fail
