// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include <android/looper.h>
#include <android/sensor.h>
#include <jni.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static bool valid_sample(const ASensorEvent* event, const ASensor* sensor, bool fixture) {
  struct timespec now;
  if (clock_gettime(CLOCK_BOOTTIME, &now) != 0) return false;
  int64_t now_ns = (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
  return event->version == sizeof(*event) && event->type == ASensor_getType(sensor) &&
      event->sensor == ASensor_getHandle(sensor) && event->timestamp > 0 &&
      event->timestamp <= now_ns && now_ns - event->timestamp < 5000000000 &&
      isfinite(event->data[0]) && isfinite(event->data[1]) && isfinite(event->data[2]) &&
      (!fixture || (event->data[0] == 0 && event->data[1] == 0 &&
                    fabsf(event->data[2] - 9.80665f) < 0.001f));
}

struct callback_probe {
  ASensorEventQueue* queue;
  const ASensor* sensor;
  bool fixture;
  bool passed;
  int calls;
};

static int on_sample(int fd, int events, void* context) {
  (void)fd;
  struct callback_probe* probe = context;
  ASensorEvent sample;
  ++probe->calls;
  probe->passed = (events & ALOOPER_EVENT_INPUT) && ASensorEventQueue_hasEvents(probe->queue) > 0 &&
      ASensorEventQueue_getEvents(probe->queue, &sample, 1) == 1 &&
      valid_sample(&sample, probe->sensor, probe->fixture);
  return 0;
}

static const char* sample_sensor(ASensorManager* manager, const ASensor* sensor, bool fixture) {
  const int ident = 41;
  ALooper* looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
  ASensorEventQueue* queue = ASensorManager_createEventQueue(manager, looper, ident, NULL, NULL);
  if (queue == NULL) return "create identifier queue";
  ASensorEvent events[8];
  int rate = ASensor_getMinDelay(sensor);
  if (rate < 20000) rate = 20000;
  const char* failure = "identifier queue delivery";
  if (ASensorEventQueue_hasEvents(queue) != 0 ||
      ASensorEventQueue_registerSensor(queue, sensor, rate, 0) != 0 ||
      ASensorEventQueue_setEventRate(queue, sensor, rate) != 0) {
    failure = "enable or set sampling rate";
  } else {
    for (int i = 0; i < 8; ++i) {
      if (ALooper_pollOnce(500, NULL, NULL, NULL) != ident) continue;
      if (ASensorEventQueue_hasEvents(queue) <= 0) break;
      ssize_t count = ASensorEventQueue_getEvents(queue, events, 8);
      if (count > 0 && valid_sample(&events[0], sensor, fixture)) failure = NULL;
      break;
    }
    if (ASensorEventQueue_disableSensor(queue, sensor) != 0) failure = "disable sensor";
  }
  if (ASensorManager_destroyEventQueue(manager, queue) != 0) return "destroy identifier queue";
  if (failure != NULL) return failure;
  struct callback_probe callback = {NULL, sensor, fixture, false, 0};
  callback.queue = ASensorManager_createEventQueue(manager, looper, ALOOPER_POLL_CALLBACK,
                                                    on_sample, &callback);
  if (callback.queue == NULL) return "create callback queue";
  if (ASensorEventQueue_enableSensor(callback.queue, sensor) == 0) {
    for (int i = 0; i < 8 && callback.calls == 0; ++i) ALooper_pollOnce(500, NULL, NULL, NULL);
  }
  if (ASensorManager_destroyEventQueue(manager, callback.queue) != 0) return "destroy callback queue";
  return callback.calls == 1 && callback.passed ? NULL : "callback queue delivery";
}

JNIEXPORT jobjectArray JNICALL Java_gesher_SensorContract_probe(JNIEnv* env, jclass cls, jstring arg) {
  (void)cls;
  const char* mode = (*env)->GetStringUTFChars(env, arg, NULL);
  if (!mode) return NULL;
  bool fixture = strcmp(mode, "fixture") == 0;
  bool require_sensor = fixture || strcmp(mode, "android") == 0;
  (*env)->ReleaseStringUTFChars(env, arg, mode);
  ASensorManager* manager = ASensorManager_getInstanceForPackage("gesher.sensors.fixture");
  ASensorList list = NULL;
  int count = ASensorManager_getSensorList(manager, &list);
  const ASensor* sensor = ASensorManager_getDefaultSensor(manager, ASENSOR_TYPE_ACCELEROMETER);
  const char* failure = NULL;
  if (count < 0) {
    failure = "read native sensor list";
  } else if (fixture) {
    sensor = NULL;
    for (int i = 0; i < count; ++i) {
      if (strcmp(ASensor_getName(list[i]), "Gesher Test Accelerometer") == 0) sensor = list[i];
    }
  }
  if (failure == NULL && require_sensor && sensor == NULL) failure = "expected accelerometer";
  if (failure == NULL && sensor != NULL) failure = sample_sensor(manager, sensor, fixture);
  if (failure != NULL) {
    jclass assertion = (*env)->FindClass(env, "java/lang/AssertionError");
    if (assertion != NULL) (*env)->ThrowNew(env, assertion, failure);
    return NULL;
  }
  jclass string_class = (*env)->FindClass(env, "java/lang/String");
  if (!string_class) return NULL;
  jobjectArray result = (*env)->NewObjectArray(env, count, string_class, NULL);
  if (!result) return NULL;
  for (int i = 0; i < count; ++i) {
    char descriptor[512];
    const ASensor* entry = list[i];
    snprintf(descriptor, sizeof(descriptor), "%d\t%s\t%s\t%s\t%d\t%d\t%d\t%d",
             ASensor_getType(entry), ASensor_getName(entry), ASensor_getVendor(entry),
             ASensor_getStringType(entry), ASensor_getMinDelay(entry), ASensor_getReportingMode(entry),
             ASensor_getFifoMaxEventCount(entry), ASensor_isWakeUpSensor(entry));
    jstring text = (*env)->NewStringUTF(env, descriptor);
    if (!text) return NULL;
    (*env)->SetObjectArrayElement(env, result, i, text);
    (*env)->DeleteLocalRef(env, text);
  }
  return result;
}
