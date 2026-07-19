#pragma once

// In-process "fake udev" for virtual input devices.
//
// SDL (hence Steam) enumerates joysticks via the udev client library: it needs a
// NETLINK_KOBJECT_UEVENT "add" event to hotplug-detect a device, and a /run/udev/data/c13:NN
// hwdb entry to classify it as ID_INPUT_JOYSTICK. This container has no udevd, so uinput-created
// pads are invisible to Steam. Wolf solves this cross-container with a fake-udev CLI + docker
// exec; we are single-container, so we inject the netlink event and write the hwdb entry directly
// from the server process. Ported from reference/wolf/src/fake-udev.
//
// NB: needs a writable /run/udev/data (see entrypoint) and NET_ADMIN (already granted) to bind
// the netlink uevent group.

#include <string>

namespace fake_udev {

// Identifies one uinput node so we can synthesize its udev event + hwdb entry, and undo them.
struct Device {
  std::string devnode; // e.g. /dev/input/event31
  std::string syspath; // /devices/... (no /sys prefix), ending in /eventNN
  unsigned int major = 13;
  unsigned int minor = 0;
  // Drives the ID_INPUT_* classification (joystick|mouse|keyboard). libinput assigns
  // device capabilities from these udev properties.
  std::string id_input_class = "joystick";
};

// Resolve a Device from a uinput fd (after UI_DEV_CREATE) via UI_GET_SYSNAME + stat. Returns false
// if the sysname/devnode can't be resolved.
bool device_from_uinput_fd(int fd, Device &out);

// Resolve a Device from an existing /dev/input/eventNN node (a device we did not create,
// e.g. Steam Input's virtual mouse/keyboard) via /sys/class/input + stat.
bool device_from_event_node(const std::string &devnode, Device &out);

// Announce (ACTION=add) or withdraw (ACTION=remove) the device to udev/SDL: writes/removes the
// /run/udev/data hwdb entry and sends the matching netlink uevent. Best-effort; logs on failure.
void plug(const Device &dev);
void unplug(const Device &dev);

} // namespace fake_udev
