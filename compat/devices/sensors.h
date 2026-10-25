// Gesher (devices track): the device's sensors (docs/devices.md), what
// SystemSensorManager's natives list and sample: Windows' own sensors
// (Windows.Devices.Sensors: accelerometer, gyrometer, magnetometer, light
// sensor, orientation sensor; whichever this PC has), and with
// GESHER_SENSOR_TEST set, test sensors whose values are known (an
// accelerometer and a light sensor), to check the whole path where no
// hardware is.
#pragma once

#include <stdint.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gesher::devices::sensors {

// android.hardware.Sensor's TYPE_* values used here.
enum Type : int {
  kAccelerometer = 1,
  kMagneticField = 2,
  kGyroscope = 4,
  kLight = 5,
  kRotationVector = 11,
};

// Sensor.mFlags' reporting modes (REPORTING_MODE_SHIFT 1).
constexpr int kContinuous = 0;
constexpr int kOnChange = 1 << 1;

struct Reading {
  float values[16] = {};
  int accuracy = 3;  // SENSOR_STATUS_ACCURACY_HIGH
};

// A source of readings: polled at the rate asked for.
class Source {
 public:
  virtual ~Source() = default;
  virtual bool Read(Reading* reading) = 0;
};

struct Sensor {
  int handle = 0;
  int type = 0;
  std::string name;
  std::string vendor;
  int version = 1;
  float maxRange = 0;
  float resolution = 0;
  float power = 0;
  int minDelayUs = 0;
  int maxDelayUs = 0;
  int flags = kContinuous;
  std::shared_ptr<Source> source;
};

// The sensors of this process, found once (Windows' and the test ones).
const std::vector<Sensor>& List();

// A consumer of events (SystemSensorManager's BaseEventQueue).
class Sink {
 public:
  virtual ~Sink() = default;
  virtual void OnEvent(int handle, const Reading& reading, int64_t timestampNs) = 0;
};

// Starts sampling `handle` for `sink` every `periodUs` (at least the sensor's
// minimum delay); again with another period changes it. Returns 0, or a
// negative errno (no such sensor).
int Enable(Sink* sink, int handle, int periodUs);
int Disable(Sink* sink, int handle);
// Removes every subscription of `sink`; after it returns, no OnEvent call
// for it runs or will run.
void DisableAll(Sink* sink);

// What Windows said about its sensors, for the logs: "accelerometer: none,
// light: none, ..." or the activation errors.
std::string Describe();

}  // namespace gesher::devices::sensors
