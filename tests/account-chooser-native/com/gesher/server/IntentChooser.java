/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
package com.gesher.server;
import java.util.function.BooleanSupplier;
/** Test-only JNI owner; never packaged in the application jar. */
final class IntentChooser {
  static native boolean nativeAvailable();
  static native int nativeChoose(String requester, String title, String[] labels, boolean[] enabled, BooleanSupplier active);
}
