// Gesher (devices track): the device natives' JNI tables (docs/devices.md),
// registered by android_runtime.cc after the stand-ins they replace:
// - com.gesher.server.WindowsLocation (gesher-app.jar): Windows'
//   location service for the system server's LocationManagerService.
// - android.hardware.SystemSensorManager and its BaseEventQueue
//   (sensors_jni.cc), replacing device_standins.cc's empty ones.
// - com.gesher.server.WindowsBluetooth: the host's Bluetooth radio
//   (bluetooth_jni.cc over bluetooth_win.cc, or bluetooth_test.cc's test radio).
// - com.gesher.server.WindowsCamera: Media Foundation's cameras (camera_mf.cc);
//   CameraMetadataNative (camera_metadata_jni.cc); ImageReader, SurfaceUtils
//   and com.gesher.server.CameraFrames (camera_frames.cc).

#include <jni.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>
#include <nativehelper/scoped_local_ref.h>
#include <nativehelper/scoped_utf_chars.h>

#include <string>
#include <vector>

#include "camera_mf.h"
#include "core_jni_helpers.h"
#include "location_win.h"

namespace android {

int RegisterGesherNatives(JNIEnv* env, const char* className, const JNINativeMethod* methods,
                          int count);  // services_natives.cc
void LogCameraDeliveryTrace();  // camera_frames.cc, opt-in recording diagnostics

namespace {

namespace loc = gesher::devices::location;

void SetResult(JNIEnv* env, jintArray hrOut, HRESULT hr) {
  if (hrOut != nullptr && env->GetArrayLength(hrOut) > 0) {
    jint value = static_cast<jint>(hr);
    env->SetIntArrayRegion(hrOut, 0, 1, &value);
  }
}

jint WindowsLocation_access(JNIEnv* env, jclass, jintArray hrOut) {
  HRESULT hr = S_OK;
  loc::Access access = loc::RequestAccess(&hr);
  SetResult(env, hrOut, hr);
  return access;
}

jint WindowsLocation_status(JNIEnv* env, jclass, jintArray hrOut) {
  HRESULT hr = S_OK;
  int status = loc::Status(&hr);
  SetResult(env, hrOut, hr);
  return status;
}

// out: latitude, longitude, accuracy, altitude (NaN: none), altitude accuracy,
// heading, speed (NaN: none), Unix time in ms, position source (-1: unknown).
constexpr int kFixValues = 9;

jint WindowsLocation_getFix(JNIEnv* env, jclass, jboolean high, jlong maxAgeMs, jlong timeoutMs,
                            jdoubleArray out) {
  if (out == nullptr || env->GetArrayLength(out) < kFixValues) return E_INVALIDARG;
  loc::Fix fix;
  HRESULT hr = loc::GetFix(high, maxAgeMs, timeoutMs, &fix);
  if (FAILED(hr)) return hr;
  const double nan = __builtin_nan("");
  jdouble values[kFixValues] = {
      fix.latitude,
      fix.longitude,
      fix.accuracy,
      fix.hasAltitude ? fix.altitude : nan,
      fix.hasAltitudeAccuracy ? fix.altitudeAccuracy : nan,
      fix.hasHeading ? fix.heading : nan,
      fix.hasSpeed ? fix.speed : nan,
      static_cast<double>(fix.timeMillis),
      static_cast<double>(fix.source),
  };
  env->SetDoubleArrayRegion(out, 0, kFixValues, values);
  return S_OK;
}

jboolean WindowsLocation_hasGnss(JNIEnv* env, jclass, jintArray hrOut) {
  HRESULT hr = S_OK;
  bool present = loc::HasGnssReceiver(&hr);
  SetResult(env, hrOut, hr);
  return present ? JNI_TRUE : JNI_FALSE;
}

const JNINativeMethod gWindowsLocationMethods[] = {
    {"nativeAccess", "([I)I", GESHER_JNI(WindowsLocation_access)},
    {"nativeStatus", "([I)I", GESHER_JNI(WindowsLocation_status)},
    {"nativeGetFix", "(ZJJ[D)I", GESHER_JNI(WindowsLocation_getFix)},
    {"nativeHasGnss", "([I)Z", GESHER_JNI(WindowsLocation_hasGnss)},
};

// --- com.gesher.server.WindowsCamera: the capture devices (camera_mf.h) ----------------

namespace cam = gesher::devices::camera;

// One string per device: "id\tname\tWxH,WxH,...\tflags", flags a comma-separated
// list of what the device really has: "af" when its focus runs automatically and
// can be locked (camera_mf.h's Device::autofocus), else empty.
jobjectArray WindowsCamera_list(JNIEnv* env, jclass) {
  const std::vector<cam::Device>& devices = cam::List(nullptr);
  jclass string = env->FindClass("java/lang/String");
  jobjectArray result = env->NewObjectArray(static_cast<jsize>(devices.size()), string, nullptr);
  for (size_t i = 0; i < devices.size(); ++i) {
    const cam::Device& d = devices[i];
    std::string line = d.id + "\t" + d.name + "\t";
    for (size_t k = 0; k < d.sizes.size(); ++k) {
      if (k > 0) line += ",";
      line += std::to_string(d.sizes[k].first) + "x" + std::to_string(d.sizes[k].second);
    }
    line += "\t";
    if (d.autofocus) line += "af";
    ScopedLocalRef<jstring> s(env, env->NewStringUTF(line.c_str()));
    env->SetObjectArrayElement(result, static_cast<jsize>(i), s.get());
  }
  return result;
}

jstring WindowsCamera_status(JNIEnv* env, jclass) {
  std::string status;
  cam::List(&status);
  return env->NewStringUTF(status.c_str());
}

// Opens a device: a handle, or 0 with out[2] = the HRESULT; out[0..1] = the frame size.
jlong WindowsCamera_open(JNIEnv* env, jclass, jstring id, jint width, jint height,
                         jintArray out) {
  ScopedUtfChars chars(env, id);
  if (chars.c_str() == nullptr) return 0;
  HRESULT hr = S_OK;
  std::unique_ptr<cam::Capture> capture = cam::Open(chars.c_str(), width, height, &hr);
  if (capture) {
    ALOGI("camera: opened %s at %dx%d through %s", chars.c_str(), capture->width,
          capture->height, capture->via);
  } else {
    ALOGI("camera: opening %s failed: HRESULT 0x%08lX", chars.c_str(),
          static_cast<unsigned long>(hr));
  }
  jint values[3] = {capture ? capture->width : 0, capture ? capture->height : 0,
                    static_cast<jint>(hr)};
  if (out != nullptr && env->GetArrayLength(out) >= 3) env->SetIntArrayRegion(out, 0, 3, values);
  return reinterpret_cast<jlong>(capture.release());
}

// The next frame (NV12) into `frame`: 0, or the failure's HRESULT.
jint WindowsCamera_read(JNIEnv* env, jclass, jlong handle, jbyteArray frame) {
  auto* capture = reinterpret_cast<cam::Capture*>(handle);
  if (capture == nullptr) return E_INVALIDARG;
  std::lock_guard<std::mutex> lock(capture->jni_read_mutex);
  cam::TraceSample trace(capture->jni_read_trace);
  const int64_t pixels = static_cast<int64_t>(capture->width) * capture->height;
  if (frame == nullptr || capture->width <= 0 || capture->height <= 0 ||
      capture->width % 2 != 0 || capture->height % 2 != 0 || pixels > INT32_MAX / 3 * 2 ||
      env->GetArrayLength(frame) < pixels * 3 / 2) {
    return E_INVALIDARG;
  }
  trace.Step(0);
  auto& buffer = capture->jni_read_buffer;
  buffer.resize(static_cast<size_t>(pixels * 3 / 2));
  trace.Step(1);
  HRESULT hr = capture->Read(buffer.data());
  trace.Step(2);
  if (SUCCEEDED(hr)) {
    env->SetByteArrayRegion(frame, 0, static_cast<jsize>(buffer.size()),
                            reinterpret_cast<const jbyte*>(buffer.data()));
  }
  trace.Step(3);
  return static_cast<jint>(hr);
}

void WindowsCamera_close(JNIEnv*, jclass, jlong handle) {
  delete reinterpret_cast<cam::Capture*>(handle);
  LogCameraDeliveryTrace();
}

// Focus control (camera_mf.h's FocusOp): 0 or the failure's HRESULT; out[0..1] = the
// lens position and the driver's flags afterwards.
jint WindowsCamera_focus(JNIEnv* env, jclass, jlong handle, jint op, jintArray out) {
  auto* capture = reinterpret_cast<cam::Capture*>(handle);
  if (capture == nullptr) return E_INVALIDARG;
  long value = 0, flags = 0;
  HRESULT hr = capture->Focus(op, &value, &flags);
  if (out != nullptr && env->GetArrayLength(out) >= 2) {
    jint values[2] = {static_cast<jint>(value), static_cast<jint>(flags)};
    env->SetIntArrayRegion(out, 0, 2, values);
  }
  return static_cast<jint>(hr);
}

const JNINativeMethod gWindowsCameraMethods[] = {
    {"nativeList", "()[Ljava/lang/String;", GESHER_JNI(WindowsCamera_list)},
    {"nativeStatus", "()Ljava/lang/String;", GESHER_JNI(WindowsCamera_status)},
    {"nativeOpen", "(Ljava/lang/String;II[I)J", GESHER_JNI(WindowsCamera_open)},
    {"nativeRead", "(J[B)I", GESHER_JNI(WindowsCamera_read)},
    {"nativeFocus", "(JI[I)I", GESHER_JNI(WindowsCamera_focus)},
    {"nativeClose", "(J)V", GESHER_JNI(WindowsCamera_close)},
};

}  // namespace

int register_gesher_sensors(JNIEnv* env);  // sensors_jni.cc
int register_gesher_camera_metadata(JNIEnv* env);  // camera_metadata_jni.cc
int register_gesher_camera_frames(JNIEnv* env);  // camera_frames.cc
int register_gesher_bluetooth(JNIEnv* env);  // bluetooth_jni.cc
int register_gesher_webauthn(JNIEnv* env);  // webauthn_jni.cc
int register_gesher_hello(JNIEnv* env);  // hello_jni.cc

int register_gesher_devices(JNIEnv* env) {
  register_gesher_sensors(env);
  register_gesher_camera_metadata(env);
  register_gesher_camera_frames(env);
  register_gesher_bluetooth(env);
  register_gesher_webauthn(env);
  register_gesher_hello(env);
  RegisterGesherNatives(env, "com/gesher/server/WindowsCamera", gWindowsCameraMethods,
                        NELEM(gWindowsCameraMethods));
  RegisterGesherNatives(env, "com/gesher/server/WindowsLocation", gWindowsLocationMethods,
                        NELEM(gWindowsLocationMethods));
  return 0;
}

}  // namespace android
