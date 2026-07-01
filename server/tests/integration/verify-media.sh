#!/usr/bin/env bash
# Media pipeline self-test: drive REST->RTSP->PLAY, assert the video appsink emits RTP.
# Audio uses audiotestsrc (STEAM_STREAM_AUDIO_TEST=1) since no pulse server runs here.
set -uo pipefail
BIN="${SERVER_BIN:-/out/steam-stream-server}"
ST=/tmp/ss-m4
rm -rf "$ST"; mkdir -p "$ST/clients"

openssl req -x509 -newkey rsa:2048 -nodes -keyout "$ST/cli.key" -out "$ST/cli.crt" \
  -days 3650 -subj "/CN=test-client" >/dev/null 2>&1
cp "$ST/cli.crt" "$ST/clients/manual.pem"

export STEAM_STREAM_STATE_DIR="$ST" STEAM_STREAM_LOG_LEVEL=INFO STEAM_STREAM_AUDIO_TEST=1
export GST_PLUGIN_PATH=/plugins
"$BIN" >"$ST/server.log" 2>&1 &
SRV=$!
sleep 2
echo "############ startup log ############"; sed -n '1,40p' "$ST/server.log"

python3 - "$ST" <<'PY'
import http.client, ssl, sys, time, socket
ST=sys.argv[1]
def https_ctx(crt,key):
    c=ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); c.check_hostname=False
    c.verify_mode=ssl.CERT_NONE; c.load_cert_chain(crt,key); return c
ctx=https_ctx(ST+"/cli.crt",ST+"/cli.key")

h=http.client.HTTPSConnection("127.0.0.1",47984,context=ctx,timeout=5)
h.request("GET","/launch?appid=1&mode=1280x720x60&rikey=9d804e9d3a7d8f1b2c3d4e5f60718293&rikeyid=16909060&rtspport=48010")
body=h.getresponse().read().decode()
import re
FAKE_IP=re.search(r"rtsp://([0-9.]+):48010",body).group(1)
print("launch -> rtsp://%s:48010"%FAKE_IP)

def rtsp(msg):
    s=socket.create_connection(("127.0.0.1",48010),timeout=5)
    s.sendall(msg.encode()); s.shutdown(socket.SHUT_WR)
    d=b""
    while True:
        c=s.recv(4096)
        if not c: break
        d+=c
    s.close(); return d.decode(errors="replace")

assert "200 OK" in rtsp("OPTIONS rtsp://%s:48010 RTSP/1.0\r\nCSeq:1\r\nHost:%s\r\n\r\n"%(FAKE_IP,FAKE_IP))
rtsp("DESCRIBE rtsp://%s:48010 RTSP/1.0\r\nCSeq:2\r\nHost:%s\r\nAccept: application/sdp\r\n\r\n"%(FAKE_IP,FAKE_IP))
for sid in ("video","audio","control"):
    rtsp("SETUP streamid=%s/0/0 RTSP/1.0\r\nCSeq:3\r\nHost:%s\r\n\r\n"%(sid,FAKE_IP))
sdp=("v=0\na=x-nv-video[0].clientViewportWd:1280 \na=x-nv-video[0].clientViewportHt:720 \n"
     "a=x-nv-video[0].maxFPS:60 \na=x-nv-video[0].packetSize:1024 \n"
     "a=x-nv-vqos[0].bw.maximumBitrateKbps:15500 \na=x-nv-vqos[0].fec.minRequiredFecPackets:2 \n"
     "a=x-nv-general.featureFlags:167 \na=x-nv-audio.surround.numChannels:2 \n"
     "a=x-nv-aqos.packetDuration:5 \na=x-nv-video[0].encoderCscMode:0 \na=x-nv-vqos[0].bitStreamFormat:0 \n")
assert "200 OK" in rtsp("ANNOUNCE streamid=control/13/0 RTSP/1.0\r\nCSeq:6\r\nHost:%s\r\n"
                        "Content-type: application/sdp\r\nContent-length:%d\r\n\r\n%s"%(FAKE_IP,len(sdp),sdp))
assert "200 OK" in rtsp("PLAY rtsp://%s:48010 RTSP/1.0\r\nCSeq:7\r\nHost:%s\r\n\r\n"%(FAKE_IP,FAKE_IP))
print("RTSP OPTIONS..PLAY -> 200 OK")
print("waiting for media pipeline to spin up + produce RTP buffers...")
time.sleep(6)
PY

kill $SRV 2>/dev/null; sleep 1
echo "############ relevant server log ############"
grep -E "control server started|PLAY for session|starting (video|audio) pipeline|appsink produced|reach PLAYING|failed" "$ST/server.log" | head -40

echo "############ assertions ############"
log=$(cat "$ST/server.log")
fail=0
grep -q "control server started on port 47999" <<<"$log" || { echo "FAIL: ENET control not bound"; fail=1; }
grep -q "PLAY for session" <<<"$log" || { echo "FAIL: PLAY hook not hit"; fail=1; }
grep -q "starting video pipeline" <<<"$log" || { echo "FAIL: video pipeline not started"; fail=1; }
grep -q "video appsink produced" <<<"$log" || { echo "FAIL: no video RTP buffers produced"; fail=1; }
[ $fail -eq 0 ] && echo "=== M4 SELF-TEST PASSED ===" || echo "=== M4 SELF-TEST FAILED ==="
exit $fail
