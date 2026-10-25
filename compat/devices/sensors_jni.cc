// Gesher (devices track): android.hardware.SystemSensorManager's natives over
// sensors.h (in place of frameworks/base's, which reach sensorservice
// through libsensor). Each event queue is an eventfd on the Java queue's
// Looper, as the framework's Receiver is a BitTube's fd: the sampling thread
// queues events and wakes the Looper, which calls
// BaseEventQueue.dispatchSensorEvent on its own thread.

#include <android_runtime/AndroidRuntime.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <nativehelper/scoped_local_ref.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utils/Looper.h>

#include <algorithm>
#include <deque>
#include <mutex>
#include <vector>

#include "android_os_MessageQueue.h"
#include "core_jni_helpers.h"
#include "sensors.h"

namespace android {

int RegisterGesherNatives(JNIEnv* env, const char* className, const JNINativeMethod* methods,
                          int count);  // services_natives.cc

namespace {

namespace hub = gesher::devices::sensors;

struct {
  jclass clazz;
  jfieldID name, vendor, version, handle, range, resolution, power, minDelay, maxDelay,
      fifoReserved, fifoMax, stringType, requiredPermission, flags;
  jmethodID setType, setId;
} gSensor;

struct {
  jmethodID dispatchSensorEvent;
  jmethodID dispatchFlushCompleteEvent;
} gQueue;

jmethodID gWeakGet;
bool gInited = false;

void SystemSensorManager_classInit(JNIEnv* env, jclass) {
  if (gInited) return;
  jclass sensor = FindClassOrDie(env, "android/hardware/Sensor");
  gSensor.clazz = MakeGlobalRefOrDie(env, sensor);
  gSensor.name = GetFieldIDOrDie(env, sensor, "mName", "Ljava/lang/String;");
  gSensor.vendor = GetFieldIDOrDie(env, sensor, "mVendor", "Ljava/lang/String;");
  gSensor.version = GetFieldIDOrDie(env, sensor, "mVersion", "I");
  gSensor.handle = GetFieldIDOrDie(env, sensor, "mHandle", "I");
  gSensor.range = GetFieldIDOrDie(env, sensor, "mMaxRange", "F");
  gSensor.resolution = GetFieldIDOrDie(env, sensor, "mResolution", "F");
  gSensor.power = GetFieldIDOrDie(env, sensor, "mPower", "F");
  gSensor.minDelay = GetFieldIDOrDie(env, sensor, "mMinDelay", "I");
  gSensor.maxDelay = GetFieldIDOrDie(env, sensor, "mMaxDelay", "I");
  gSensor.fifoReserved = GetFieldIDOrDie(env, sensor, "mFifoReservedEventCount", "I");
  gSensor.fifoMax = GetFieldIDOrDie(env, sensor, "mFifoMaxEventCount", "I");
  gSensor.stringType = GetFieldIDOrDie(env, sensor, "mStringType", "Ljava/lang/String;");
  gSensor.requiredPermission =
      GetFieldIDOrDie(env, sensor, "mRequiredPermission", "Ljava/lang/String;");
  gSensor.flags = GetFieldIDOrDie(env, sensor, "mFlags", "I");
  gSensor.setType = GetMethodIDOrDie(env, sensor, "setType", "(I)Z");
  gSensor.setId = GetMethodIDOrDie(env, sensor, "setId", "(I)V");
  jclass queue = FindClassOrDie(env, "android/hardware/SystemSensorManager$BaseEventQueue");
  gQueue.dispatchSensorEvent = GetMethodIDOrDie(env, queue, "dispatchSensorEvent", "(I[FIJ)V");
  gQueue.dispatchFlushCompleteEvent =
      GetMethodIDOrDie(env, queue, "dispatchFlushCompleteEvent", "(I)V");
  jclass weak = FindClassOrDie(env, "java/lang/ref/WeakReference");
  gWeakGet = GetMethodIDOrDie(env, weak, "get", "()Ljava/lang/Object;");
  gInited = true;
  ALOGI("sensors: %s", hub::Describe().c_str());
}

jlong SystemSensorManager_create(JNIEnv*, jclass, jstring) {
  hub::List();
  return 1;  // one device-wide sensor list: nothing per manager
}

jboolean FillSensor(JNIEnv* env, jobject sensor, jint index) {
  const std::vector<hub::Sensor>& list = hub::List();
  if (index < 0 || static_cast<size_t>(index) >= list.size()) return JNI_FALSE;
  const hub::Sensor& s = list[index];
  ScopedLocalRef<jstring> name(env, env->NewStringUTF(s.name.c_str()));
  ScopedLocalRef<jstring> vendor(env, env->NewStringUTF(s.vendor.c_str()));
  env->SetObjectField(sensor, gSensor.name, name.get());
  env->SetObjectField(sensor, gSensor.vendor, vendor.get());
  env->SetIntField(sensor, gSensor.version, s.version);
  env->SetIntField(sensor, gSensor.handle, s.handle);
  env->SetFloatField(sensor, gSensor.range, s.maxRange);
  env->SetFloatField(sensor, gSensor.resolution, s.resolution);
  env->SetFloatField(sensor, gSensor.power, s.power);
  env->SetIntField(sensor, gSensor.minDelay, s.minDelayUs);
  env->SetIntField(sensor, gSensor.maxDelay, s.maxDelayUs);
  env->SetIntField(sensor, gSensor.fifoReserved, 0);
  env->SetIntField(sensor, gSensor.fifoMax, 0);
  env->SetObjectField(sensor, gSensor.requiredPermission, nullptr);
  env->SetIntField(sensor, gSensor.flags, s.flags);
  env->CallBooleanMethod(sensor, gSensor.setType, s.type);
  env->CallVoidMethod(sensor, gSensor.setId, s.handle);
  return JNI_TRUE;
}

jboolean SystemSensorManager_getSensorAtIndex(JNIEnv* env, jclass, jlong, jobject sensor,
                                              jint index) {
  return FillSensor(env, sensor, index);
}

void SystemSensorManager_getDynamicSensors(JNIEnv*, jclass, jlong, jobject) {
  // No dynamic sensors.
}

void SystemSensorManager_getRuntimeSensors(JNIEnv*, jclass, jlong, jint, jobject) {
  // No virtual devices.
}

jboolean SystemSensorManager_false(JNIEnv*, jclass, jlong) { return JNI_FALSE; }

jint SystemSensorManager_createDirectChannel(JNIEnv*, jclass, jlong, jint, jlong, jint, jint,
                                             jobject) {
  return -EINVAL;  // no direct channels
}

void SystemSensorManager_destroyDirectChannel(JNIEnv*, jclass, jlong, jint) {}

jint SystemSensorManager_configDirectChannel(JNIEnv*, jclass, jlong, jint, jint, jint) {
  return -EINVAL;
}

jint SystemSensorManager_setOperationParameter(JNIEnv*, jclass, jlong, jint, jint, jfloatArray,
                                               jintArray) {
  return -EINVAL;
}

// --- event queues ----------------------------------------------------------------------

struct Event {
  int handle;
  bool flush;
  hub::Reading reading;
  int64_t timestampNs;
};

class Queue : public LooperCallback, public hub::Sink {
 public:
  Queue(JNIEnv* env, jobject weak, const sp<Looper>& looper)
      : weak_(env->NewGlobalRef(weak)), looper_(looper) {
    fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    scratch_ = static_cast<jfloatArray>(env->NewGlobalRef(env->NewFloatArray(16)));
  }

  bool ok() const { return fd_ >= 0; }

  void Start() { looper_->addFd(fd_, 0, Looper::EVENT_INPUT, this, nullptr); }

  // Ends the sampling and the Looper's watch; the references go with the last sp
  // (the Looper keeps one while it calls handleEvent).
  void Destroy() {
    hub::DisableAll(this);
    looper_->removeFd(fd_);
  }

  ~Queue() override {
    if (fd_ >= 0) close(fd_);
    JNIEnv* env = AndroidRuntime::getJNIEnv();
    if (env != nullptr) {
      env->DeleteGlobalRef(weak_);
      env->DeleteGlobalRef(scratch_);
    }
  }

  // The sampling thread.
  void OnEvent(int handle, const hub::Reading& reading, int64_t timestampNs) override {
    Push({handle, false, reading, timestampNs});
  }

  void Flush(const std::vector<int>& handles) {
    for (int h : handles) Push({h, true, hub::Reading(), 0});
  }

  std::vector<int> enabled;

 private:
  void Push(const Event& e) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (events_.size() >= 256) events_.pop_front();  // a slow consumer drops the oldest
      events_.push_back(e);
    }
    uint64_t one = 1;
    if (write(fd_, &one, sizeof(one)) < 0) {
      // The counter is full or the queue is gone: the Looper is awake anyway.
    }
  }

  // The Looper's thread.
  int handleEvent(int fd, int, void*) override {
    uint64_t count;
    while (read(fd, &count, sizeof(count)) > 0) {
    }
    std::deque<Event> events;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      events.swap(events_);
    }
    JNIEnv* env = AndroidRuntime::getJNIEnv();
    ScopedLocalRef<jobject> queue(env, env->CallObjectMethod(weak_, gWeakGet));
    if (queue.get() == nullptr) return 1;
    for (const Event& e : events) {
      if (e.flush) {
        env->CallVoidMethod(queue.get(), gQueue.dispatchFlushCompleteEvent, e.handle);
      } else {
        env->SetFloatArrayRegion(scratch_, 0, 16, e.reading.values);
        env->CallVoidMethod(queue.get(), gQueue.dispatchSensorEvent, e.handle, scratch_,
                            e.reading.accuracy, static_cast<jlong>(e.timestampNs));
      }
      if (env->ExceptionCheck()) {
        ALOGE("sensors: exception dispatching a sensor event");
        env->ExceptionDescribe();
        env->ExceptionClear();
        break;
      }
    }
    return 1;
  }

  jobject weak_;
  jfloatArray scratch_;
  sp<Looper> looper_;
  int fd_ = -1;
  std::mutex mutex_;
  std::deque<Event> events_;
};

jlong BaseEventQueue_init(JNIEnv* env, jclass, jlong, jobject weak, jobject messageQueue, jstring,
                          jint mode, jstring, jstring) {
  if (mode != 0) {
    jniThrowException(env, "java/lang/UnsupportedOperationException",
                      "sensor data injection is not supported on Gesher");
    return 0;
  }
  sp<MessageQueue> mq = android_os_MessageQueue_getMessageQueue(env, messageQueue);
  if (mq == nullptr) {
    jniThrowRuntimeException(env, "MessageQueue is not initialized.");
    return 0;
  }
  sp<Queue> q = sp<Queue>::make(env, weak, mq->getLooper());
  if (!q->ok()) {
    jniThrowRuntimeException(env, "Cannot construct native SensorEventQueue.");
    return 0;
  }
  q->Start();
  q->incStrong(reinterpret_cast<void*>(BaseEventQueue_init));
  return reinterpret_cast<jlong>(q.get());
}

Queue* QueueOf(jlong eventQ) { return reinterpret_cast<Queue*>(eventQ); }

jint BaseEventQueue_enable(JNIEnv*, jclass, jlong eventQ, jint handle, jint rateUs, jint) {
  Queue* q = QueueOf(eventQ);
  int r = hub::Enable(q, handle, rateUs);
  if (r == 0 && std::find(q->enabled.begin(), q->enabled.end(), handle) == q->enabled.end()) {
    q->enabled.push_back(handle);
  }
  return r;
}

jint BaseEventQueue_disable(JNIEnv*, jclass, jlong eventQ, jint handle) {
  Queue* q = QueueOf(eventQ);
  q->enabled.erase(std::remove(q->enabled.begin(), q->enabled.end(), handle), q->enabled.end());
  return hub::Disable(q, handle);
}

void BaseEventQueue_destroy(JNIEnv*, jclass, jlong eventQ) {
  Queue* q = QueueOf(eventQ);
  q->Destroy();
  q->decStrong(reinterpret_cast<void*>(BaseEventQueue_init));
}

jint BaseEventQueue_flush(JNIEnv*, jclass, jlong eventQ) {
  Queue* q = QueueOf(eventQ);
  q->Flush(q->enabled);
  return 0;
}

jint BaseEventQueue_inject(JNIEnv*, jclass, jlong, jint, jfloatArray, jint, jlong) {
  return -EINVAL;
}

#define STR "Ljava/lang/String;"

const JNINativeMethod gSystemSensorManagerMethods[] = {
    {"nativeClassInit", "()V", GESHER_JNI(SystemSensorManager_classInit)},
    {"nativeCreate", "(" STR ")J", GESHER_JNI(SystemSensorManager_create)},
    {"nativeGetSensorAtIndex", "(JLandroid/hardware/Sensor;I)Z",
     GESHER_JNI(SystemSensorManager_getSensorAtIndex)},
    {"nativeGetDefaultDeviceSensorAtIndex", "(JLandroid/hardware/Sensor;I)Z",
     GESHER_JNI(SystemSensorManager_getSensorAtIndex)},
    {"nativeGetDynamicSensors", "(JLjava/util/List;)V",
     GESHER_JNI(SystemSensorManager_getDynamicSensors)},
    {"nativeGetRuntimeSensors", "(JILjava/util/List;)V",
     GESHER_JNI(SystemSensorManager_getRuntimeSensors)},
    {"nativeIsDataInjectionEnabled", "(J)Z", GESHER_JNI(SystemSensorManager_false)},
    {"nativeIsReplayDataInjectionEnabled", "(J)Z", GESHER_JNI(SystemSensorManager_false)},
    {"nativeIsHalBypassReplayDataInjectionEnabled", "(J)Z",
     GESHER_JNI(SystemSensorManager_false)},
    {"nativeCreateDirectChannel", "(JIJIILandroid/hardware/HardwareBuffer;)I",
     GESHER_JNI(SystemSensorManager_createDirectChannel)},
    {"nativeDestroyDirectChannel", "(JI)V", GESHER_JNI(SystemSensorManager_destroyDirectChannel)},
    {"nativeConfigDirectChannel", "(JIII)I", GESHER_JNI(SystemSensorManager_configDirectChannel)},
    {"nativeSetOperationParameter", "(JII[F[I)I",
     GESHER_JNI(SystemSensorManager_setOperationParameter)},
};

const JNINativeMethod gBaseEventQueueMethods[] = {
    {"nativeInitBaseEventQueue",
     "(JLjava/lang/ref/WeakReference;Landroid/os/MessageQueue;" STR "I" STR STR ")J",
     GESHER_JNI(BaseEventQueue_init)},
    {"nativeEnableSensor", "(JIII)I", GESHER_JNI(BaseEventQueue_enable)},
    {"nativeDisableSensor", "(JI)I", GESHER_JNI(BaseEventQueue_disable)},
    {"nativeDestroySensorEventQueue", "(J)V", GESHER_JNI(BaseEventQueue_destroy)},
    {"nativeFlushSensor", "(J)I", GESHER_JNI(BaseEventQueue_flush)},
    {"nativeInjectSensorData", "(JI[FIJ)I", GESHER_JNI(BaseEventQueue_inject)},
};

}  // namespace

int register_gesher_sensors(JNIEnv* env) {
  RegisterGesherNatives(env, "android/hardware/SystemSensorManager", gSystemSensorManagerMethods,
                        NELEM(gSystemSensorManagerMethods));
  RegisterGesherNatives(env, "android/hardware/SystemSensorManager$BaseEventQueue",
                        gBaseEventQueueMethods, NELEM(gBaseEventQueueMethods));
  return 0;
}

}  // namespace android
