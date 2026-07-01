#pragma once

// Raw-uinput virtual input devices. Container must run with --device /dev/uinput.

#include <cstdint>
#include <memory>
#include <string>

namespace input {

bool uinput_available();

// moonlight_key_to_linux returns -1 for an unmapped key.
unsigned int moonlight_button_to_linux(int moonlight_button);
int moonlight_key_to_linux(short moonlight_key);

class VirtualMouse {
public:
  static std::unique_ptr<VirtualMouse> create();
  ~VirtualMouse();

  void move_rel(int dx, int dy);
  // x/y already scaled into the [0, screen_w/h) device range.
  void move_abs(int x, int y);
  void set_abs_range(int screen_w, int screen_h);

  // Moonlight button ids: 1=left 2=middle 3=right 4=side 5=extra.
  void button(int moonlight_button, bool pressed);
  void vscroll(int amount);
  void hscroll(int amount);

private:
  explicit VirtualMouse(int fd) : fd_(fd) {}
  int fd_ = -1;
};

class VirtualKeyboard {
public:
  static std::unique_ptr<VirtualKeyboard> create();
  ~VirtualKeyboard();

  void key(short moonlight_key, bool pressed);

private:
  explicit VirtualKeyboard(int fd) : fd_(fd) {}
  int fd_ = -1;
};

class VirtualGamepad {
public:
  static std::unique_ptr<VirtualGamepad> create();
  ~VirtualGamepad();

  // triggers 0..255, sticks -32768..32767.
  void update(std::uint32_t button_flags,
              std::uint8_t left_trigger,
              std::uint8_t right_trigger,
              short left_x,
              short left_y,
              short right_x,
              short right_y);

private:
  explicit VirtualGamepad(int fd) : fd_(fd) {}
  int fd_ = -1;
};

} // namespace input
