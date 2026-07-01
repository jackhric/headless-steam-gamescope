#!/usr/bin/env bash
# Runs inside the builder container. Exercises the steam-stream-server endpoints.
set -uo pipefail
BIN="${SERVER_BIN:-/out/steam-stream-server}"
ST=/tmp/ss-state
rm -rf "$ST"; mkdir -p "$ST/clients"

# Pre-seed a paired client cert.
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$ST/cli.key" -out "$ST/cli.crt" \
  -days 3650 -subj "/CN=test-client" >/dev/null 2>&1
cp "$ST/cli.crt" "$ST/clients/manual.pem"
# An unpaired client cert (NOT in the store).
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$ST/bad.key" -out "$ST/bad.crt" \
  -days 3650 -subj "/CN=evil-client" >/dev/null 2>&1

export STEAM_STREAM_STATE_DIR="$ST"
export STEAM_STREAM_HTTP_PORT=47989
export STEAM_STREAM_HTTPS_PORT=47984
export STEAM_STREAM_LOG_LEVEL=INFO
"$BIN" >"$ST/server.log" 2>&1 &
SRV=$!
sleep 2

echo "############ server.log (startup) ############"
cat "$ST/server.log"

python3 - <<'PY'
import http.client, ssl, sys, threading, time, json, urllib.parse, subprocess

def banner(t): print("\n############ %s ############" % t)

# 1) HTTP /serverinfo (unpaired)
banner("HTTP GET /serverinfo")
c = http.client.HTTPConnection("127.0.0.1", 47989, timeout=5)
c.request("GET", "/serverinfo"); r = c.getresponse(); body = r.read().decode()
print("status:", r.status); print(body)
assert r.status == 200 and "SUNSHINE_SERVER_FREE" in body and "<PairStatus>0" in body, "serverinfo HTTP failed"

# 2) HTTP /pair phase1 + async PIN flow
banner("HTTP /pair phase1 + /pin POST (async PIN wiring)")
cli_pem = open("/tmp/ss-state/cli.crt","rb").read()
cert_hex = cli_pem.hex()
salt = "a0c288cfb0ea624ec3e5cc54d6ab7e38"
def do_phase1(out):
    cc = http.client.HTTPConnection("127.0.0.1", 47989, timeout=15)
    q = urllib.parse.urlencode({"uniqueid":"0123456789","salt":salt,"clientcert":cert_hex})
    cc.request("GET", "/pair?"+q); rr = cc.getresponse(); out.append((rr.status, rr.read().decode()))
res=[]
t = threading.Thread(target=do_phase1, args=(res,)); t.start()
# Find the pairing secret from the log, then POST the PIN.
secret=None
for _ in range(50):
    time.sleep(0.1)
    for line in open("/tmp/ss-state/server.log"):
        if "/pin/#" in line:
            secret = line.split("/pin/#")[1].split()[0].strip(); break
    if secret: break
print("pin secret:", secret); assert secret, "no PIN secret logged"
pc = http.client.HTTPConnection("127.0.0.1", 47989, timeout=5)
pc.request("POST","/pin/", body=json.dumps({"pin":"1234","secret":secret}),
           headers={"Content-Type":"application/json"})
pr = pc.getresponse(); print("POST /pin/ ->", pr.status, pr.read().decode())
t.join(timeout=10)
print("phase1 response:", res[0][0]); print(res[0][1])
assert res[0][0]==200 and "<plaincert>" in res[0][1] and "<paired>1" in res[0][1], "phase1 failed"

def https_ctx(cert, key):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname=False; ctx.verify_mode=ssl.CERT_NONE
    ctx.load_cert_chain(cert, key); return ctx

# 3) HTTPS /serverinfo with the PAIRED (pre-seeded) client cert
banner("HTTPS GET /serverinfo (paired client cert)")
ctx = https_ctx("/tmp/ss-state/cli.crt","/tmp/ss-state/cli.key")
h = http.client.HTTPSConnection("127.0.0.1", 47984, context=ctx, timeout=5)
h.request("GET","/serverinfo"); r=h.getresponse(); body=r.read().decode()
print("status:", r.status); print(body)
assert r.status==200 and "<PairStatus>1" in body, "paired HTTPS serverinfo failed"

# 4) HTTPS /applist
banner("HTTPS GET /applist (paired)")
h = http.client.HTTPSConnection("127.0.0.1", 47984, context=ctx, timeout=5)
h.request("GET","/applist"); r=h.getresponse(); body=r.read().decode()
print("status:", r.status); print(body)
assert "Steam Big Picture" in body and "Steam Desktop" in body, "applist failed"

# 5) HTTPS /launch (stub)
banner("HTTPS GET /launch?appid=1 (REAL session)")
import re
h = http.client.HTTPSConnection("127.0.0.1", 47984, context=ctx, timeout=5)
RIKEY="9d804e47a6aa6624b7d4b502b32cc522"; RIKEYID="16909060"
h.request("GET","/launch?appid=1&mode=1920x1080x60&rikey=%s&rikeyid=%s&surroundAudioInfo=196610"%(RIKEY,RIKEYID))
r=h.getresponse(); body=r.read().decode()
print("status:", r.status); print(body)
assert "rtsp://" in body and "<gamesession>1" in body, "launch failed"
m = re.search(r"rtsp://([0-9.]+):48010", body); assert m, "no rtsp url in launch response"
FAKE_IP = m.group(1)
print("session rtsp fake ip:", FAKE_IP)

# 5b) serverinfo now reports a running session (BUSY + currentgame).
banner("HTTPS GET /serverinfo (now BUSY)")
h = http.client.HTTPSConnection("127.0.0.1", 47984, context=ctx, timeout=5)
h.request("GET","/serverinfo"); r=h.getresponse(); body=r.read().decode()
print(body)
assert "SUNSHINE_SERVER_BUSY" in body and "<currentgame>1" in body, "serverinfo should be BUSY after launch"

# 5c) LIVE RTSP over a raw TCP socket on :48010, using the fake IP Moonlight parrots back.
banner("LIVE RTSP OPTIONS/DESCRIBE/SETUP/ANNOUNCE/PLAY on :48010")
import socket
def rtsp(msg):
    s = socket.create_connection(("127.0.0.1", 48010), timeout=5)
    s.sendall(msg.encode()); s.shutdown(socket.SHUT_WR)
    data=b""
    while True:
        chunk=s.recv(4096)
        if not chunk: break
        data+=chunk
    s.close(); return data.decode(errors="replace")

opt = rtsp("OPTIONS rtsp://%s:48010 RTSP/1.0\r\nCSeq: 1\r\nHost: %s\r\n\r\n"%(FAKE_IP,FAKE_IP))
print("OPTIONS ->", opt.splitlines()[0]); assert "200 OK" in opt and "CSeq: 1" in opt

desc = rtsp("DESCRIBE rtsp://%s:48010 RTSP/1.0\r\nCSeq: 2\r\nHost: %s\r\nAccept: application/sdp\r\n\r\n"%(FAKE_IP,FAKE_IP))
print("DESCRIBE ->", desc.splitlines()[0])
assert "200 OK" in desc and "surround-params=21101" in desc and "x-ss-general.featureFlags" in desc, "DESCRIBE SDP wrong"

setup_v = rtsp("SETUP streamid=video/0/0 RTSP/1.0\r\nCSeq: 3\r\nHost: %s\r\n\r\n"%FAKE_IP)
print("SETUP video ->", setup_v.splitlines()[0])
assert "200 OK" in setup_v and "server_port=48100" in setup_v, "SETUP video wrong port"
setup_a = rtsp("SETUP streamid=audio/0/0 RTSP/1.0\r\nCSeq: 4\r\nHost: %s\r\n\r\n"%FAKE_IP)
assert "200 OK" in setup_a and "server_port=48200" in setup_a, "SETUP audio wrong port"
setup_c = rtsp("SETUP streamid=control/0/0 RTSP/1.0\r\nCSeq: 5\r\nHost: %s\r\n\r\n"%FAKE_IP)
assert "200 OK" in setup_c and "server_port=47999" in setup_c, "SETUP control wrong port"
print("SETUP video/audio/control -> 48100/48200/47999 OK")

sdp = ("v=0\n"
       "a=x-nv-video[0].clientViewportWd:1920 \n"
       "a=x-nv-video[0].clientViewportHt:1080 \n"
       "a=x-nv-video[0].maxFPS:60 \n"
       "a=x-nv-video[0].packetSize:1024 \n"
       "a=x-nv-vqos[0].bw.maximumBitrateKbps:15500 \n"
       "a=x-nv-vqos[0].fec.minRequiredFecPackets:2 \n"
       "a=x-nv-general.featureFlags:167 \n"
       "a=x-nv-audio.surround.numChannels:2 \n"
       "a=x-nv-aqos.packetDuration:5 \n"
       "a=x-nv-video[0].encoderCscMode:0 \n"
       "a=x-nv-vqos[0].bitStreamFormat:0 \n")
ann = ("ANNOUNCE streamid=control/13/0 RTSP/1.0\r\nCSeq: 6\r\nHost: %s\r\n"
       "Content-type: application/sdp\r\nContent-length: %d\r\n\r\n%s"%(FAKE_IP,len(sdp),sdp))
ann_r = rtsp(ann)
print("ANNOUNCE ->", ann_r.splitlines()[0]); assert "200 OK" in ann_r and "CSeq: 6" in ann_r, "ANNOUNCE failed"

play = rtsp("PLAY rtsp://%s:48010 RTSP/1.0\r\nCSeq: 7\r\nHost: %s\r\n\r\n"%(FAKE_IP,FAKE_IP))
print("PLAY ->", play.splitlines()[0]); assert "200 OK" in play, "PLAY failed"
time.sleep(0.3)
log = open("/tmp/ss-state/server.log").read()
assert "ANNOUNCE captured: 1920x1080@60" in log, "server did not capture ANNOUNCE params"
assert "PLAY for session" in log, "server did not hit PLAY hook"
print("RTSP exchange OK; server captured ANNOUNCE params + hit the M4 PLAY hook")

# 6) HTTPS /serverinfo with an UNPAIRED cert -> 401
banner("HTTPS GET /serverinfo (UNPAIRED client cert -> expect 401)")
ctx2 = https_ctx("/tmp/ss-state/bad.crt","/tmp/ss-state/bad.key")
h = http.client.HTTPSConnection("127.0.0.1", 47984, context=ctx2, timeout=5)
h.request("GET","/serverinfo"); r=h.getresponse(); body=r.read().decode()
print("status:", r.status); print(body)
assert r.status==401 and "not authorized" in body, "unpaired rejection failed"

print("\n=== ALL ENDPOINT CHECKS PASSED ===")
PY
RC=$?
kill $SRV 2>/dev/null
exit $RC
