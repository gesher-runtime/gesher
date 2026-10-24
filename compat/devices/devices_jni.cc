// Gesher (devices track): the device natives' JNI tables (docs/devices.md),
// registered by android_runtime.cc after the stand-ins they replace:
// - com.gesher.server.WindowsLocation (gesher-app.jar): Windows'
//   location service for the system server's LocationManagerService.
// - android.hardware.SystemSensorManager and its BaseEventQueue
//   (sensors_jni.cc), replacing device_standins.cc's empty ones.

#include <jni.h>
#include <nativehelper/JNIHelp.h>

#include "core_jni_helpers.h"
#include "location_win.h"

namespace android {

int RegisterGesherNatives(JNIEnv* env, const char* className, const JNINativeMethod* methods,
                          int count);  // services_natives.cc

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

const JNINativeMethod gWindowsLocationMethods[] = {
    {"nativeAccess", "([I)I", GESHER_JNI(WindowsLocation_access)},
    {"nativeStatus", "([I)I", GESHER_JNI(WindowsLocation_status)},
    {"nativeGetFix", "(ZJJ[D)I", GESHER_JNI(WindowsLocation_getFix)},
};

}  // namespace

int register_gesher_sensors(JNIEnv* env);  // sensors_jni.cc

int register_gesher_devices(JNIEnv* env) {
  register_gesher_sensors(env);
  RegisterGesherNatives(env, "com/gesher/server/WindowsLocation", gWindowsLocationMethods,
                        NELEM(gWindowsLocationMethods));
  return 0;
}

}  // namespace android
