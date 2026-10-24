// Gesher (devices track): the cameras of this PC through Media Foundation
// (video capture devices, read with an IMFSourceReader as NV12), and with
// GESHER_CAMERA_TEST set a test camera whose frames are known, below the
// camera service (docs/devices.md). Media Foundation's DLLs are loaded at
// run time: a Windows edition without them has no cameras, cleanly.
#pragma once

#include <windows.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace gesher::devices::camera {

struct Device {
  std::string id;    // the symbolic link, or "gesher-test"
  std::string name;  // the friendly name
  bool test = false;
  std::vector<std::pair<int, int>> sizes;  // frame sizes it offers, largest first
};

// The capture devices (enumerated once per process; `status` says what Media
// Foundation answered, for the logs).
const std::vector<Device>& List(std::string* status);

// An open device, producing NV12 frames of width x height.
class Capture {
 public:
  virtual ~Capture() = default;
  // The next frame into `nv12` (width * height * 3 / 2 bytes); blocks until
  // there is one. S_OK, or the failure.
  virtual HRESULT Read(uint8_t* nv12) = 0;
  int width = 0, height = 0;
  const char* via = "";  // how frames are read, for the logs
};

// Opens `id` at the offered size nearest to width x height.
std::unique_ptr<Capture> Open(const std::string& id, int width, int height, HRESULT* hr);

}  // namespace gesher::devices::camera
