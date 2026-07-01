// GameStream XML correctness for the REST responses (serverinfo / applist / launch).

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <cstdio>
#include <server/moonlight_proto.hpp>
#include <sstream>
#include <string>

namespace pt = boost::property_tree;

static int failures = 0;
#define CHECK(cond, name)                                                                                              \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::printf("  [FAIL] %s\n", name);                                                                              \
      failures++;                                                                                                      \
    } else {                                                                                                           \
      std::printf("  [ OK ] %s\n", name);                                                                             \
    }                                                                                                                  \
  } while (0)

static std::string render(const pt::ptree &xml) {
  std::ostringstream ss;
  pt::write_xml(ss, xml);
  return ss.str();
}

// Serialise then re-parse: throws if the emitted XML is not well-formed.
static bool well_formed(const pt::ptree &xml, pt::ptree &out) {
  try {
    std::istringstream in(render(xml));
    pt::read_xml(in, out);
    return true;
  } catch (...) {
    return false;
  }
}

int main() {
  std::printf("== serverinfo XML ==\n");
  {
    std::vector<moonlight::DisplayMode> modes = {{1920, 1080, 60}, {1280, 720, 60}};
    auto xml = moonlight::serverinfo(/*busy*/ false, /*appid*/ 0, 47984, 47989, "uuid-1234", "steam-stream-host",
                                     "AA:BB:CC:DD:EE:FF", "127.0.0.1", modes, /*pair_status*/ 0,
                                     /*hevc*/ true, /*av1*/ false);
    pt::ptree rp;
    CHECK(well_formed(xml, rp), "serverinfo XML is well-formed");
    CHECK(rp.get<int>("root.<xmlattr>.status_code") == 200, "status_code 200");
    CHECK(rp.get<std::string>("root.hostname") == "steam-stream-host", "hostname field");
    CHECK(rp.get<std::string>("root.appversion") == moonlight::M_VERSION, "appversion field");
    CHECK(rp.get<int>("root.HttpsPort") == 47984, "HttpsPort 47984");
    CHECK(rp.get<int>("root.ExternalPort") == 47989, "ExternalPort 47989");
    CHECK(rp.get<int>("root.PairStatus") == 0, "PairStatus 0 (unpaired)");
    CHECK(rp.get<int>("root.currentgame") == 0, "currentgame 0");
    CHECK(rp.get<std::string>("root.state") == "SUNSHINE_SERVER_FREE", "state FREE when idle");
    // H264(0x1)|HEVC(0x100)=257.
    CHECK(rp.get<int>("root.ServerCodecModeSupport") == 257, "codec support H264+HEVC = 257");
    CHECK(rp.get<long>("root.MaxLumaPixelsHEVC") == 1869449984L, "HEVC max luma pixels set");
    int n_modes = 0;
    for (auto &c : rp.get_child("root.SupportedDisplayMode"))
      if (c.first == "DisplayMode")
        n_modes++;
    CHECK(n_modes == 2, "two SupportedDisplayMode entries");
  }

  std::printf("\n== serverinfo XML (busy) ==\n");
  {
    std::vector<moonlight::DisplayMode> modes = {{1280, 720, 60}};
    auto xml = moonlight::serverinfo(true, 1, 47984, 47989, "u", "h", "m", "127.0.0.1", modes, 1, false, false);
    pt::ptree rp;
    CHECK(well_formed(xml, rp), "busy serverinfo XML is well-formed");
    CHECK(rp.get<std::string>("root.state") == "SUNSHINE_SERVER_BUSY", "state BUSY with a session");
    CHECK(rp.get<int>("root.currentgame") == 1, "currentgame 1");
    CHECK(rp.get<int>("root.PairStatus") == 1, "PairStatus 1 (paired)");
    CHECK(rp.get<int>("root.ServerCodecModeSupport") == 1, "codec support H264-only = 1 (no hevc/av1)");
  }

  std::printf("\n== applist XML ==\n");
  {
    std::vector<moonlight::App> apps = {{"Steam Big Picture", "1", false}, {"Steam Desktop", "2", false}};
    auto xml = moonlight::applist(apps);
    pt::ptree rp;
    CHECK(well_formed(xml, rp), "applist XML is well-formed");
    int n = 0;
    bool saw_bp = false, saw_desk = false;
    for (auto &child : rp.get_child("root")) {
      if (child.first != "App")
        continue;
      n++;
      auto title = child.second.get<std::string>("AppTitle");
      auto id = child.second.get<std::string>("ID");
      if (title == "Steam Big Picture" && id == "1")
        saw_bp = true;
      if (title == "Steam Desktop" && id == "2")
        saw_desk = true;
    }
    CHECK(n == 2, "two App entries");
    CHECK(saw_bp, "Steam Big Picture (ID 1) present");
    CHECK(saw_desk, "Steam Desktop (ID 2) present");
  }

  std::printf("\n== launch XML ==\n");
  {
    auto xml = moonlight::launch_success("17.229.248.129", "48010");
    pt::ptree rp;
    CHECK(well_formed(xml, rp), "launch XML is well-formed");
    CHECK(rp.get<std::string>("root.sessionUrl0") == "rtsp://17.229.248.129:48010", "sessionUrl0 RTSP url");
    CHECK(rp.get<int>("root.gamesession") == 1, "gamesession 1");
  }

  std::printf(failures == 0 ? "\nALL XML TESTS PASSED\n" : "\n%d XML TEST(S) FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
