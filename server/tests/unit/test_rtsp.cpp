// Known-answer test for the RTSP verb handlers, driving real Moonlight-client request
// strings. Also checks the /launch AES key/iv byte format the rtpmoonlightpay element expects.

#include <array>
#include <boost/endian/conversion.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <crypto/crypto.hpp>
#include <server/rtsp_commands.hpp>
#include <string>

using namespace std::string_literals;

static int failures = 0;
#define CHECK(cond, name)                                                                                              \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::printf("  [FAIL] %s\n", name);                                                                             \
      failures++;                                                                                                      \
    } else {                                                                                                          \
      std::printf("  [ OK ] %s\n", name);                                                                             \
    }                                                                                                                  \
  } while (0)

#define CHECK_EQ(a, b, name)                                                                                           \
  do {                                                                                                                 \
    auto _va = (a);                                                                                                    \
    auto _vb = (b);                                                                                                    \
    if (_va != _vb) {                                                                                                  \
      std::printf("  [FAIL] %s\n    expected: %s\n    got:      %s\n", name, std::string(_vb).c_str(),                 \
                  std::string(_va).c_str());                                                                          \
      failures++;                                                                                                      \
    } else {                                                                                                          \
      std::printf("  [ OK ] %s\n", name);                                                                             \
    }                                                                                                                  \
  } while (0)

static session::StreamSession test_session() {
  session::StreamSession s;
  s.session_id = 1234;
  s.client_ip = "127.0.0.1";
  s.rtsp_fake_ip = "00.11.22.33.44";
  s.hevc_supported = true;
  s.av1_supported = false;
  s.video_stream_port = 1234;
  s.audio_stream_port = 1235;
  s.control_stream_port = 1236;
  s.audio_channel_count = 2;
  s.rtp_secret_payload.fill('A');
  s.enet_secret_payload = 42;
  return s;
}

// Round-trip the response through to_string()+parse(): payloads like "sprop-parameter-sets=AAAAAU"
// only split into key/value once they've crossed the wire.
static rtsp::RTSP_PACKET handle(const std::string &raw, session::StreamSession &s) {
  auto parsed = rtsp::parse(raw);
  if (!parsed) {
    std::printf("  [FAIL] could not parse request:\n%s\n", raw.c_str());
    failures++;
    return rtsp::error_msg(400, "PARSE FAIL");
  }
  auto resp = rtsp::message_handler(*parsed, s);
  auto reparsed = rtsp::parse(rtsp::to_string(resp));
  if (!reparsed) {
    std::printf("  [FAIL] response did not round-trip through the wire\n");
    failures++;
    return resp;
  }
  return *reparsed;
}

int main() {
  std::printf("== RTSP parser round-trip ==\n");
  {
    auto raw = "OPTIONS rtsp://10.1.2.49:48010 RTSP/1.0\nCSeq: 1\nX-GS-ClientVersion: 14\nHost: 10.1.2.49\r\n\r\n"s;
    auto p = rtsp::parse(raw);
    CHECK(p.has_value(), "parse OPTIONS");
    CHECK(rtsp::to_string(*p) == rtsp::to_string(*rtsp::parse(rtsp::to_string(*p))), "OPTIONS round-trips");
    CHECK(!rtsp::parse("OPTIONS rtsp://10.1.2.49:48010 RTSP/1.0").has_value(), "missing CSeq rejected");
  }

  std::printf("\n== Verbs (KAT vs Wolf testRTSP.cpp) ==\n");
  auto s = test_session();

  {
    auto r = handle("OPTIONS rtsp://00.11.22.33.44:48010 RTSP/1.0\r\nCSeq: 1\r\n"
                    "X-GS-ClientVersion: 14\r\nHost: 0.0.0.0\r\n\r\n"s, s);
    CHECK(r.response.status_code == 200 && r.seq_number == 1, "OPTIONS -> 200 CSeq 1");
  }

  {
    auto r = handle("DESCRIBE rtsp://00.11.22.33.44:48010 RTSP/1.0\nCSeq: 2\n"
                    "X-GS-ClientVersion: 14\nHost: 00.11.22.33.44\nAccept: application/sdp\r\n\r\n"s, s);
    CHECK(r.response.status_code == 200 && r.seq_number == 2, "DESCRIBE -> 200 CSeq 2");
    CHECK(r.payloads.size() == 5, "DESCRIBE 5 payloads");
    CHECK_EQ(r.payloads[0].first, "sprop-parameter-sets"s, "payload[0] key");
    CHECK_EQ(r.payloads[0].second, "AAAAAU"s, "payload[0] sprop");
    CHECK_EQ(r.payloads[1].second, "fmtp:97 surround-params=21101"s, "stereo surround-params");
    CHECK_EQ(r.payloads[2].second, "fmtp:97 surround-params=642014235"s, "5.1 surround-params");
    CHECK_EQ(r.payloads[3].second, "fmtp:97 surround-params=85301423675"s, "7.1 surround-params");
    CHECK_EQ(r.payloads[4].second, "x-ss-general.featureFlags: 3"s, "featureFlags");
  }

  {
    auto r = handle("SETUP streamid=audio/0/0 RTSP/1.0\nCSeq: 3\nHost: 00.11.22.33.44\r\n\r\n"s, s);
    CHECK(r.response.status_code == 200 && r.seq_number == 3, "SETUP audio -> 200");
    CHECK_EQ(r.options["Session"], "DEADBEEFCAFE;timeout = 90"s, "audio Session");
    CHECK_EQ(r.options["Transport"], "server_port=1235"s, "audio Transport port");
  }
  {
    auto r = handle("SETUP streamid=video/0/0 RTSP/1.0\nCSeq: 4\nHost: 00.11.22.33.44\r\n\r\n"s, s);
    CHECK(r.response.status_code == 200 && r.seq_number == 4, "SETUP video -> 200");
    CHECK_EQ(r.options["Transport"], "server_port=1234"s, "video Transport port");
  }
  {
    auto r = handle("SETUP streamid=control/0/0 RTSP/1.0\nCSeq: 5\nHost: 00.11.22.33.44\r\n\r\n"s, s);
    CHECK(r.response.status_code == 200 && r.seq_number == 5, "SETUP control -> 200");
    CHECK_EQ(r.options["Transport"], "server_port=1236"s, "control Transport port");
    CHECK_EQ(r.options["X-SS-Connect-Data"], "42"s, "control enet secret");
  }

  {
    // The full ANNOUNCE SDP a real client sends.
    auto r = handle("ANNOUNCE streamid=control/13/0 RTSP/1.0\nCSeq: 6\nX-GS-ClientVersion: 14\n"
                    "Host: 00.11.22.33.44\nSession:  DEADBEEFCAFE\nContent-type: application/sdp\n"
                    "Content-length: 1308\r\n\r\n"
                    "v=0\no=android 0 14 IN IPv4 0.0.0.0\ns=NVIDIA Streaming Client\n"
                    "a=x-nv-video[0].clientViewportWd:1920 \na=x-nv-video[0].clientViewportHt:1080 \n"
                    "a=x-nv-video[0].maxFPS:60 \na=x-nv-video[0].packetSize:1024 \n"
                    "a=x-nv-video[0].rateControlMode:4 \na=x-nv-video[0].timeoutLengthMs:7000 \n"
                    "a=x-nv-video[0].framesWithInvalidRefThreshold:0 \na=x-nv-video[0].initialBitrateKbps:15500 \n"
                    "a=x-nv-video[0].initialPeakBitrateKbps:15500 \na=x-nv-vqos[0].bw.minimumBitrateKbps:15500 \n"
                    "a=x-nv-vqos[0].bw.maximumBitrateKbps:15500 \na=x-nv-vqos[0].fec.enable:1 \n"
                    "a=x-nv-vqos[0].videoQualityScoreUpdateTime:5000 \na=x-nv-vqos[0].qosTrafficType:0 \n"
                    "a=x-nv-aqos.qosTrafficType:0 \na=x-nv-general.featureFlags:167 \n"
                    "a=x-nv-general.useReliableUdp:13 \na=x-nv-vqos[0].fec.minRequiredFecPackets:2 \n"
                    "a=x-nv-vqos[0].drc.enable:0 \na=x-nv-general.enableRecoveryMode:0 \n"
                    "a=x-nv-video[0].videoEncoderSlicesPerFrame:1 \na=x-nv-clientSupportHevc:0 \n"
                    "a=x-nv-vqos[0].bitStreamFormat:0 \na=x-nv-video[0].dynamicRangeMode:0 \n"
                    "a=x-nv-video[0].maxNumReferenceFrames:1 \na=x-nv-video[0].clientRefreshRateX100:0 \n"
                    "a=x-nv-audio.surround.numChannels:2 \na=x-nv-audio.surround.channelMask:3 \n"
                    "a=x-nv-audio.surround.enable:0 \na=x-nv-audio.surround.AudioQuality:0 \n"
                    "a=x-nv-aqos.packetDuration:5 \na=x-nv-video[0].encoderCscMode:0 \n"
                    "t=0 0\nm=video 47998 \n"s, s);
    CHECK(r.response.status_code == 200 && r.seq_number == 6, "ANNOUNCE -> 200 CSeq 6");
    CHECK(s.announced, "session marked announced");
    CHECK(s.video.width == 1920 && s.video.height == 1080 && s.video.fps == 60, "video mode 1920x1080@60");
    CHECK(s.video.format == session::VideoFormat::H264, "video format H264 (bitStreamFormat 0)");
    CHECK(s.video.packet_size == 1024, "video packet_size 1024");
    CHECK(s.video.bitrate_kbps == 15500, "video bitrate 15500 (no x-ml override)");
    CHECK(s.video.min_required_fec_packets == 2, "min fec packets 2");
    CHECK(s.video.slices_per_frame == 1, "slices per frame 1");
    CHECK(s.video.color_range == session::ColorRange::MPEG, "color range MPEG (csc 0)");
    CHECK(s.audio.channels == 2, "audio channels 2");
    CHECK(s.audio.encrypt == true, "audio encrypt true (featureFlags 167 & 0x20 == 0x20)");
  }

  {
    auto r = handle("PLAY rtsp://00.11.22.33.44:48010 RTSP/1.0\r\nCSeq: 7\r\nHost: 00.11.22.33.44\r\n\r\n"s, s);
    CHECK(r.response.status_code == 200 && r.seq_number == 7, "PLAY -> 200 CSeq 7");
    CHECK(s.playing, "session marked playing");
  }

  {
    auto r = handle("MissingNo rtsp://00.11.22.33.44:48010 RTSP/1.0\r\nCSeq: 1\r\n\r\n"s, s);
    CHECK(r.response.status_code == 404, "unknown verb -> 404");
  }

  std::printf("\n== AES key/iv capture matches rtpmoonlightpay element format ==\n");
  {
    session::StreamSession ls;
    ls.aes_key = "9d804e47a6aa6624b7d4b502b32cc522"; // rikey: 32 hex chars
    ls.aes_iv = "16909060";                          // rikeyid: decimal uint32 (== 0x01020304)

    auto raw_key = crypto::hex_to_str(ls.aes_key, true);
    CHECK(raw_key.size() == 16, "rikey hex-decodes to a 16-byte AES-128 key");

    std::array<std::uint8_t, 16> iv{};
    std::uint32_t input_iv = std::stoul(ls.aes_iv);
    int cur_seq = 0;
    *reinterpret_cast<std::uint32_t *>(iv.data()) = boost::endian::native_to_big(input_iv + cur_seq);
    CHECK(iv[0] == 0x01 && iv[1] == 0x02 && iv[2] == 0x03 && iv[3] == 0x04, "rikeyid -> big-endian IV first 4 bytes");
    CHECK(iv[4] == 0 && iv[15] == 0, "IV remainder zero-filled");
  }

  std::printf(failures == 0 ? "\nALL RTSP TESTS PASSED\n" : "\n%d RTSP TEST(S) FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
