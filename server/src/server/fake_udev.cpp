#include <server/fake_udev.hpp>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <helpers/logger.hpp>
#include <linux/netlink.h>
#include <linux/uinput.h>
#include <map>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace fake_udev {

namespace {

const char *kUdevDataDir = "/run/udev/data";

// MurmurHash2 (public domain, Austin Appleby) -- libudev hashes the subsystem string with this
// into the netlink header so kernel socket filters can match it. Must be byte-identical to
// systemd's, so this is copied verbatim from Wolf's fake-udev.
std::uint32_t murmur2(const void *key, int len, std::uint32_t seed) {
  const std::uint32_t m = 0x5bd1e995;
  const int r = 24;
  std::uint32_t h = seed ^ len;
  const unsigned char *data = static_cast<const unsigned char *>(key);
  while (len >= 4) {
    std::uint32_t k;
    std::memcpy(&k, data, 4);
    k *= m;
    k ^= k >> r;
    k *= m;
    h *= m;
    h ^= k;
    data += 4;
    len -= 4;
  }
  switch (len) {
  case 3: h ^= data[2] << 16; [[fallthrough]];
  case 2: h ^= data[1] << 8;  [[fallthrough]];
  case 1: h ^= data[0];
    h *= m;
  }
  h ^= h >> 13;
  h *= m;
  h ^= h >> 15;
  return h;
}

// systemd device-monitor netlink header (see sd-device/device-monitor.c). Field order/sizes are
// ABI with libudev subscribers, so it must match exactly.
struct MonitorNetlinkHeader {
  char prefix[8] = "libudev";
  unsigned magic = 0xfeedcafe;
  unsigned header_size;
  unsigned properties_off;
  unsigned properties_len;
  unsigned filter_subsystem_hash;
  unsigned filter_devtype_hash;
  unsigned filter_tag_bloom_hi;
  unsigned filter_tag_bloom_lo;
};

// The udev "add"/"remove" event properties. Mirrors Wolf's gen_udev_base_event + the joystick
// tags SDL keys off of.
std::map<std::string, std::string> base_event(const Device &dev, const char *action) {
  auto now = std::chrono::system_clock::now();
  auto ts = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
  return {
      {"ACTION", action},
      {"SEQNUM", "7"}, // no global seqnum state; udev subscribers don't require monotonicity here
      {"USEC_INITIALIZED", std::to_string(ts)},
      {"SUBSYSTEM", "input"},
      {"ID_INPUT", "1"},
      {"ID_INPUT_JOYSTICK", "1"},
      {"ID_SERIAL", "noserial"},
      {"TAGS", ":seat:uaccess:"},
      {"CURRENT_TAGS", ":seat:uaccess:"},
      {"DEVNAME", dev.devnode},
      {"DEVPATH", dev.syspath},
      {"MAJOR", std::to_string(dev.major)},
      {"MINOR", std::to_string(dev.minor)},
  };
}

// udev property payload: NUL-separated KEY=VALUE pairs, trailing NUL.
std::string serialize_props(const std::map<std::string, std::string> &props) {
  std::string out;
  for (const auto &[k, v] : props) {
    out += k;
    out += '=';
    out += v;
    out += '\0';
  }
  return out;
}

// UDEV_MONITOR_UDEV: the multicast group libudev/SDL subscribers listen on. (Group 1 is the raw
// KERNEL group, which libudev subscribers ignore.)
constexpr unsigned kUdevMonitorGroup = 2;

bool send_uevent(const std::map<std::string, std::string> &props) {
  int fd = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
  if (fd < 0) {
    logs::log(logs::warning, "[FAKE-UDEV] netlink socket failed: {}", std::strerror(errno));
    return false;
  }
  // Bind with groups=0: we are a SENDER, not a subscriber. The multicast target group is set on the
  // destination address below. (Binding to the group would instead subscribe us to it.)
  sockaddr_nl src{};
  src.nl_family = AF_NETLINK;
  src.nl_pid = 0; // let the kernel assign a unique unicast pid
  if (::bind(fd, reinterpret_cast<sockaddr *>(&src), sizeof(src)) < 0) {
    logs::log(logs::warning, "[FAKE-UDEV] netlink bind failed: {}", std::strerror(errno));
    ::close(fd);
    return false;
  }

  std::string payload = serialize_props(props);
  MonitorNetlinkHeader header{};
  header.magic = htobe32(0xfeedcafe);
  header.header_size = sizeof(header);
  header.properties_off = sizeof(header);
  header.properties_len = static_cast<unsigned>(payload.size());
  header.filter_subsystem_hash = htobe32(murmur2("input", 5, 0));

  iovec iov[2] = {
      {&header, sizeof(header)},
      {payload.data(), payload.size()},
  };
  // Multicast to the UDEV monitor group: nl_pid=0 + nl_groups=<group> means "send to the group",
  // which requires CAP_NET_ADMIN (granted). This is how libudev broadcasts synthesized events.
  sockaddr_nl dst{};
  dst.nl_family = AF_NETLINK;
  dst.nl_pid = 0;
  dst.nl_groups = kUdevMonitorGroup;
  msghdr msg{};
  msg.msg_name = &dst;
  msg.msg_namelen = sizeof(dst);
  msg.msg_iov = iov;
  msg.msg_iovlen = 2;

  bool ok = ::sendmsg(fd, &msg, 0) > 0;
  if (!ok)
    logs::log(logs::warning, "[FAKE-UDEV] sendmsg failed: {}", std::strerror(errno));
  ::close(fd);
  return ok;
}

std::string hwdb_path(const Device &dev) {
  return std::string(kUdevDataDir) + "/c" + std::to_string(dev.major) + ":" +
         std::to_string(dev.minor);
}

void write_hwdb(const Device &dev) {
  std::error_code ec;
  std::filesystem::create_directories(kUdevDataDir, ec);
  auto path = hwdb_path(dev);
  std::ofstream f(path, std::ios::trunc);
  if (!f) {
    logs::log(logs::warning, "[FAKE-UDEV] cannot write hwdb {}: {}", path, std::strerror(errno));
    return;
  }
  // Matches Wolf's get_udev_hw_db_entries for a joystick. E: exported props, G/Q: tags, V: version.
  f << "E:ID_INPUT=1\n"
       "E:ID_INPUT_JOYSTICK=1\n"
       "E:ID_BUS=usb\n"
       "G:seat\n"
       "G:uaccess\n"
       "Q:seat\n"
       "Q:uaccess\n"
       "V:1\n";
}

void remove_hwdb(const Device &dev) {
  std::error_code ec;
  std::filesystem::remove(hwdb_path(dev), ec);
}

} // namespace

bool device_from_uinput_fd(int fd, Device &out) {
  char sysname[128] = {0};
  if (::ioctl(fd, UI_GET_SYSNAME(sizeof(sysname)), sysname) < 0) {
    logs::log(logs::warning, "[FAKE-UDEV] UI_GET_SYSNAME failed: {}", std::strerror(errno));
    return false;
  }
  // sysname is like "input42"; the event node lives under its sysfs dir. Find the eventNN child.
  std::string sys_input = std::string("/sys/devices/virtual/input/") + sysname;
  std::string event_name;
  std::error_code ec;
  for (const auto &e : std::filesystem::directory_iterator(sys_input, ec)) {
    auto n = e.path().filename().string();
    if (n.rfind("event", 0) == 0) {
      event_name = n;
      break;
    }
  }
  if (event_name.empty()) {
    logs::log(logs::warning, "[FAKE-UDEV] no eventNN under {}", sys_input);
    return false;
  }

  out.devnode = "/dev/input/" + event_name;
  // DEVPATH is the syspath without the /sys prefix, pointing at the event node.
  out.syspath = "/devices/virtual/input/" + std::string(sysname) + "/" + event_name;

  struct stat st{};
  if (::stat(out.devnode.c_str(), &st) == 0) {
    out.major = major(st.st_rdev);
    out.minor = minor(st.st_rdev);
  } else {
    logs::log(logs::warning, "[FAKE-UDEV] stat {} failed: {}", out.devnode, std::strerror(errno));
    return false;
  }
  return true;
}

void plug(const Device &dev) {
  write_hwdb(dev);
  bool sent = send_uevent(base_event(dev, "add"));
  logs::log(logs::info, "[FAKE-UDEV] plug {} (c{}:{}) uevent={}", dev.devnode, dev.major, dev.minor,
            sent ? "sent" : "FAILED");
}

void unplug(const Device &dev) {
  send_uevent(base_event(dev, "remove"));
  remove_hwdb(dev);
  logs::log(logs::info, "[FAKE-UDEV] unplug {}", dev.devnode);
}

} // namespace fake_udev
