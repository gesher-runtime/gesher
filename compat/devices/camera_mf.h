// Gesher (devices track): the cameras of this PC through Media Foundation
// (video capture devices, read with an IMFSourceReader as NV12), and with
// GESHER_CAMERA_TEST set a test camera whose frames are known, below the
// camera service (docs/devices.md). Media Foundation's DLLs are loaded at
// run time: a Windows edition without them has no cameras, cleanly.
#pragma once

#include <windows.h>
#include <stdint.h>

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "camera_trace.h"

namespace gesher::devices::camera {

struct Device {
  std::string id;    // the symbolic link, or "gesher-test"
  std::string name;  // the friendly name
  bool test = false;
  // The driver's focus control (IAMCameraControl's CameraControl_Focus) offers
  // automatic focus and a manual position, so focus can run and be locked. The
  // test camera has fixed focus.
  bool autofocus = false;
  std::vector<std::pair<int, int>> sizes;  // frame sizes it offers, largest first
};

// What Capture::Focus does.
enum FocusOp {
  kFocusGet = 0,   // the lens position and the driver's flags (1 auto, 2 manual)
  kFocusAuto = 1,  // the driver's automatic focus on
  kFocusLock = 2,  // automatic focus off, the lens kept where it is
};

// The capture devices (enumerated once per process; `status` says what Media
// Foundation answered, for the logs).
const std::vector<Device>& List(std::string* status);

// An open device, producing NV12 frames of width x height.
class Capture {
 public:
  virtual ~Capture() { jni_read_trace.Log(); }
  // The next frame into `nv12` (width * height * 3 / 2 bytes); blocks until
  // there is one. S_OK, or the failure.
  virtual HRESULT Read(uint8_t* nv12) = 0;
  // Controls the device's focus (FocusOp); `value` and `flags` receive the lens
  // position and the driver's flags afterwards. E_NOTIMPL without a focus control.
  virtual HRESULT Focus(int op, long* value, long* flags) {
    (void)op;
    *value = 0;
    *flags = 0;
    return E_NOTIMPL;
  }
  int width = 0, height = 0;
  const char* via = "";  // how frames are read, for the logs
  // nativeRead keeps one scratch frame for this capture. Its mutex also keeps
  // a second caller from changing bytes before the first has published them.
  // Width/height are fixed after opening; destruction releases the scratch.
  std::mutex jni_read_mutex;
  std::vector<uint8_t> jni_read_buffer;
  TraceStats jni_read_trace{"JNI read", {"validate", "prepare buffer", "capture", "copy to Java"}};
};

// Opens `id` at the offered size nearest to width x height.
std::unique_ptr<Capture> Open(const std::string& id, int width, int height, HRESULT* hr);

}  // namespace gesher::devices::camera
