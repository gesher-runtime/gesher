/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Test-only JVM boundaries. Focus state and dispatcher delivery are real Java. */
#include "../notification/framework_host.c"
/* AudioService initializes unrelated stream-volume state in its constructor. */
JNIEXPORT jint JNICALL Java_android_media_AudioSystem_initStreamVolume(JNIEnv* env,jclass cls,jint stream,jint minimum,jint maximum) { return 0; }
/* AudioSystem's static audio-format limits are not consulted by focus handling. */
JNIEXPORT jint JNICALL Java_android_media_AudioSystem_native_1getMaxChannelCount(JNIEnv* env,jclass cls) { return 24; }
JNIEXPORT jint JNICALL Java_android_media_AudioSystem_native_1getMaxSampleRate(JNIEnv* env,jclass cls) { return 192000; }
JNIEXPORT jint JNICALL Java_android_media_AudioSystem_native_1getMinSampleRate(JNIEnv* env,jclass cls) { return 4000; }
