#include <server/uinput.hpp>

#include <cstring>
#include <fcntl.h>
#include <helpers/logger.hpp>
#include <linux/input-event-codes.h>
#include <linux/uinput.h>
#include <map>
#include <unistd.h>

namespace input {

namespace {

void emit(int fd, std::uint16_t type, std::uint16_t code, std::int32_t value) {
  if (fd < 0)
    return;
  struct input_event ev {};
  ev.type = type;
  ev.code = code;
  ev.value = value;
  if (::write(fd, &ev, sizeof(ev)) != sizeof(ev))
    logs::log(logs::warning, "[UINPUT] write failed: {}", std::strerror(errno));
}

void syn(int fd) {
  emit(fd, EV_SYN, SYN_REPORT, 0);
}

int open_uinput() {
  int fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (fd < 0)
    fd = ::open("/dev/input/uinput", O_WRONLY | O_NONBLOCK);
  if (fd < 0)
    logs::log(logs::error, "[UINPUT] cannot open /dev/uinput: {} (need --device /dev/uinput)",
              std::strerror(errno));
  return fd;
}

bool finish_create(int fd, const char *name, std::uint16_t vendor, std::uint16_t product) {
  struct uinput_setup usetup {};
  usetup.id.bustype = BUS_USB;
  usetup.id.vendor = vendor;
  usetup.id.product = product;
  usetup.id.version = 1;
  std::strncpy(usetup.name, name, UINPUT_MAX_NAME_SIZE - 1);
  if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0) {
    logs::log(logs::error, "[UINPUT] UI_DEV_SETUP failed for {}: {}", name, std::strerror(errno));
    return false;
  }
  if (ioctl(fd, UI_DEV_CREATE) < 0) {
    logs::log(logs::error, "[UINPUT] UI_DEV_CREATE failed for {}: {}", name, std::strerror(errno));
    return false;
  }
  logs::log(logs::info, "[UINPUT] created virtual device: {}", name);
  return true;
}

// Moonlight/Windows VK code -> linux evdev keycode.
const std::map<short, int> &key_mappings() {
  static const std::map<short, int> m = {
      {0x08, KEY_BACKSPACE}, {0x09, KEY_TAB},        {0x0C, KEY_CLEAR},      {0x0D, KEY_ENTER},
      {0x10, KEY_LEFTSHIFT}, {0x11, KEY_LEFTCTRL},   {0x12, KEY_LEFTALT},    {0x13, KEY_PAUSE},
      {0x14, KEY_CAPSLOCK},  {0x1B, KEY_ESC},        {0x20, KEY_SPACE},      {0x21, KEY_PAGEUP},
      {0x22, KEY_PAGEDOWN},  {0x23, KEY_END},        {0x24, KEY_HOME},       {0x25, KEY_LEFT},
      {0x26, KEY_UP},        {0x27, KEY_RIGHT},      {0x28, KEY_DOWN},       {0x2C, KEY_SYSRQ},
      {0x2D, KEY_INSERT},    {0x2E, KEY_DELETE},     {0x30, KEY_0},          {0x31, KEY_1},
      {0x32, KEY_2},         {0x33, KEY_3},          {0x34, KEY_4},          {0x35, KEY_5},
      {0x36, KEY_6},         {0x37, KEY_7},          {0x38, KEY_8},          {0x39, KEY_9},
      {0x41, KEY_A},         {0x42, KEY_B},          {0x43, KEY_C},          {0x44, KEY_D},
      {0x45, KEY_E},         {0x46, KEY_F},          {0x47, KEY_G},          {0x48, KEY_H},
      {0x49, KEY_I},         {0x4A, KEY_J},          {0x4B, KEY_K},          {0x4C, KEY_L},
      {0x4D, KEY_M},         {0x4E, KEY_N},          {0x4F, KEY_O},          {0x50, KEY_P},
      {0x51, KEY_Q},         {0x52, KEY_R},          {0x53, KEY_S},          {0x54, KEY_T},
      {0x55, KEY_U},         {0x56, KEY_V},          {0x57, KEY_W},          {0x58, KEY_X},
      {0x59, KEY_Y},         {0x5A, KEY_Z},          {0x5B, KEY_LEFTMETA},   {0x5C, KEY_RIGHTMETA},
      {0x60, KEY_KP0},       {0x61, KEY_KP1},        {0x62, KEY_KP2},        {0x63, KEY_KP3},
      {0x64, KEY_KP4},       {0x65, KEY_KP5},        {0x66, KEY_KP6},        {0x67, KEY_KP7},
      {0x68, KEY_KP8},       {0x69, KEY_KP9},        {0x6A, KEY_KPASTERISK}, {0x6B, KEY_KPPLUS},
      {0x6D, KEY_KPMINUS},   {0x6E, KEY_KPDOT},      {0x6F, KEY_KPSLASH},    {0x70, KEY_F1},
      {0x71, KEY_F2},        {0x72, KEY_F3},         {0x73, KEY_F4},         {0x74, KEY_F5},
      {0x75, KEY_F6},        {0x76, KEY_F7},         {0x77, KEY_F8},         {0x78, KEY_F9},
      {0x79, KEY_F10},       {0x7A, KEY_F11},        {0x7B, KEY_F12},        {0x90, KEY_NUMLOCK},
      {0x91, KEY_SCROLLLOCK},{0xA0, KEY_LEFTSHIFT},  {0xA1, KEY_RIGHTSHIFT}, {0xA2, KEY_LEFTCTRL},
      {0xA3, KEY_RIGHTCTRL}, {0xA4, KEY_LEFTALT},    {0xA5, KEY_RIGHTALT},   {0xBA, KEY_SEMICOLON},
      {0xBB, KEY_EQUAL},     {0xBC, KEY_COMMA},      {0xBD, KEY_MINUS},      {0xBE, KEY_DOT},
      {0xBF, KEY_SLASH},     {0xC0, KEY_GRAVE},      {0xDB, KEY_LEFTBRACE},  {0xDC, KEY_BACKSLASH},
      {0xDD, KEY_RIGHTBRACE},{0xDE, KEY_APOSTROPHE}, {0xE2, KEY_102ND},
  };
  return m;
}

} // namespace

bool uinput_available() {
  return ::access("/dev/uinput", W_OK) == 0 || ::access("/dev/input/uinput", W_OK) == 0;
}

unsigned int moonlight_button_to_linux(int moonlight_button) {
  switch (moonlight_button) {
  case 1: return BTN_LEFT;
  case 2: return BTN_MIDDLE;
  case 3: return BTN_RIGHT;
  case 4: return BTN_SIDE;
  default: return BTN_EXTRA;
  }
}

int moonlight_key_to_linux(short moonlight_key) {
  auto it = key_mappings().find(moonlight_key);
  return it == key_mappings().end() ? -1 : it->second;
}

std::unique_ptr<VirtualMouse> VirtualMouse::create() {
  int fd = open_uinput();
  if (fd < 0)
    return nullptr;

  ioctl(fd, UI_SET_EVBIT, EV_KEY);
  for (int b : {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA})
    ioctl(fd, UI_SET_KEYBIT, b);

  ioctl(fd, UI_SET_EVBIT, EV_REL);
  for (int r : {REL_X, REL_Y, REL_WHEEL, REL_HWHEEL})
    ioctl(fd, UI_SET_RELBIT, r);

  ioctl(fd, UI_SET_EVBIT, EV_ABS);
  for (int a : {ABS_X, ABS_Y}) {
    ioctl(fd, UI_SET_ABSBIT, a);
    struct uinput_abs_setup abs {};
    abs.code = a;
    abs.absinfo.minimum = 0;
    abs.absinfo.maximum = 65535;
    ioctl(fd, UI_ABS_SETUP, &abs);
  }

  if (!finish_create(fd, "steam-stream virtual mouse", 0xDEAD, 0x0001)) {
    ::close(fd);
    return nullptr;
  }
  return std::unique_ptr<VirtualMouse>(new VirtualMouse(fd));
}

VirtualMouse::~VirtualMouse() {
  if (fd_ >= 0) {
    ioctl(fd_, UI_DEV_DESTROY);
    ::close(fd_);
  }
}

void VirtualMouse::set_abs_range(int, int) {} // fixed 0..65535 range; caller scales

void VirtualMouse::move_rel(int dx, int dy) {
  if (dx)
    emit(fd_, EV_REL, REL_X, dx);
  if (dy)
    emit(fd_, EV_REL, REL_Y, dy);
  syn(fd_);
}

void VirtualMouse::move_abs(int x, int y) {
  emit(fd_, EV_ABS, ABS_X, x);
  emit(fd_, EV_ABS, ABS_Y, y);
  syn(fd_);
}

void VirtualMouse::button(int moonlight_button, bool pressed) {
  emit(fd_, EV_KEY, moonlight_button_to_linux(moonlight_button), pressed ? 1 : 0);
  syn(fd_);
}

void VirtualMouse::vscroll(int amount) {
  emit(fd_, EV_REL, REL_WHEEL, amount > 0 ? 1 : (amount < 0 ? -1 : 0));
  syn(fd_);
}

void VirtualMouse::hscroll(int amount) {
  emit(fd_, EV_REL, REL_HWHEEL, amount > 0 ? 1 : (amount < 0 ? -1 : 0));
  syn(fd_);
}

std::unique_ptr<VirtualKeyboard> VirtualKeyboard::create() {
  int fd = open_uinput();
  if (fd < 0)
    return nullptr;

  ioctl(fd, UI_SET_EVBIT, EV_KEY);
  ioctl(fd, UI_SET_KEYBIT, KEY_BACKSPACE);
  for (const auto &[vk, code] : key_mappings())
    ioctl(fd, UI_SET_KEYBIT, code);

  if (!finish_create(fd, "steam-stream virtual keyboard", 0xDEAD, 0x0002)) {
    ::close(fd);
    return nullptr;
  }
  return std::unique_ptr<VirtualKeyboard>(new VirtualKeyboard(fd));
}

VirtualKeyboard::~VirtualKeyboard() {
  if (fd_ >= 0) {
    ioctl(fd_, UI_DEV_DESTROY);
    ::close(fd_);
  }
}

void VirtualKeyboard::key(short moonlight_key, bool pressed) {
  auto it = key_mappings().find(moonlight_key);
  if (it == key_mappings().end()) {
    logs::log(logs::debug, "[UINPUT] unmapped keycode 0x{:x}", moonlight_key);
    return;
  }
  emit(fd_, EV_KEY, it->second, pressed ? 1 : 0);
  syn(fd_);
}

// Moonlight CONTROLLER_BTN flags (subset) -> evdev BTN codes.
namespace {
struct PadBtn {
  std::uint32_t ml;
  int code;
};
const PadBtn kPadButtons[] = {
    {0x0010, BTN_START},  {0x0020, BTN_SELECT}, {0x0400, BTN_MODE},
    {0x0040, BTN_THUMBL}, {0x0080, BTN_THUMBR}, {0x0100, BTN_TL},
    {0x0200, BTN_TR},     {0x1000, BTN_A},      {0x2000, BTN_B},
    {0x4000, BTN_X},      {0x8000, BTN_Y},
};
} // namespace

std::unique_ptr<VirtualGamepad> VirtualGamepad::create() {
  int fd = open_uinput();
  if (fd < 0)
    return nullptr;

  ioctl(fd, UI_SET_EVBIT, EV_KEY);
  for (const auto &b : kPadButtons)
    ioctl(fd, UI_SET_KEYBIT, b.code);

  ioctl(fd, UI_SET_EVBIT, EV_ABS);
  auto abs_axis = [&](int code, int min, int max) {
    ioctl(fd, UI_SET_ABSBIT, code);
    struct uinput_abs_setup abs {};
    abs.code = code;
    abs.absinfo.minimum = min;
    abs.absinfo.maximum = max;
    ioctl(fd, UI_ABS_SETUP, &abs);
  };
  abs_axis(ABS_X, -32768, 32767);
  abs_axis(ABS_Y, -32768, 32767);
  abs_axis(ABS_RX, -32768, 32767);
  abs_axis(ABS_RY, -32768, 32767);
  abs_axis(ABS_Z, 0, 255);
  abs_axis(ABS_RZ, 0, 255);
  // D-pad as hat axes.
  abs_axis(ABS_HAT0X, -1, 1);
  abs_axis(ABS_HAT0Y, -1, 1);

  if (!finish_create(fd, "steam-stream virtual gamepad", 0x045E, 0x02EA)) {
    ::close(fd);
    return nullptr;
  }
  return std::unique_ptr<VirtualGamepad>(new VirtualGamepad(fd));
}

VirtualGamepad::~VirtualGamepad() {
  if (fd_ >= 0) {
    ioctl(fd_, UI_DEV_DESTROY);
    ::close(fd_);
  }
}

void VirtualGamepad::update(std::uint32_t button_flags,
                            std::uint8_t left_trigger,
                            std::uint8_t right_trigger,
                            short left_x,
                            short left_y,
                            short right_x,
                            short right_y) {
  for (const auto &b : kPadButtons)
    emit(fd_, EV_KEY, b.code, (button_flags & b.ml) ? 1 : 0);

  int hat_x = (button_flags & 0x0008 ? 1 : 0) - (button_flags & 0x0004 ? 1 : 0); // RIGHT - LEFT
  int hat_y = (button_flags & 0x0002 ? 1 : 0) - (button_flags & 0x0001 ? 1 : 0); // DOWN - UP
  emit(fd_, EV_ABS, ABS_HAT0X, hat_x);
  emit(fd_, EV_ABS, ABS_HAT0Y, hat_y);

  emit(fd_, EV_ABS, ABS_X, left_x);
  emit(fd_, EV_ABS, ABS_Y, -left_y - 1); // evdev Y is inverted vs Moonlight
  emit(fd_, EV_ABS, ABS_RX, right_x);
  emit(fd_, EV_ABS, ABS_RY, -right_y - 1);
  emit(fd_, EV_ABS, ABS_Z, left_trigger);
  emit(fd_, EV_ABS, ABS_RZ, right_trigger);
  syn(fd_);
}

} // namespace input
