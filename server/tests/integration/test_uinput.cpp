// Smoke test for the raw-uinput injector. Needs a container run with --device /dev/uinput.

#include <helpers/logger.hpp>
#include <server/uinput.hpp>
#include <thread>

int main() {
  logs::init(logs::info);
  if (!input::uinput_available()) {
    logs::log(logs::error, "/dev/uinput not available -- run with --device /dev/uinput");
    return 1;
  }

  auto mouse = input::VirtualMouse::create();
  auto kbd = input::VirtualKeyboard::create();
  auto pad = input::VirtualGamepad::create();
  if (!mouse || !kbd || !pad) {
    logs::log(logs::error, "failed to create one or more virtual devices");
    return 1;
  }

  mouse->move_rel(10, -5);
  mouse->button(1, true);
  mouse->button(1, false);
  kbd->key(0x41, true); // 'A'
  kbd->key(0x41, false);
  pad->update(0x1000 /*A*/, 0, 0, 1000, -1000, 0, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  logs::log(logs::info, "uinput smoke test OK: mouse + keyboard + gamepad created and driven");
  return 0;
}
