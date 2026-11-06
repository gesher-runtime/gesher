package com.gesher.server;

import java.util.function.BooleanSupplier;

/** Test-only JNI owner, never included in the application jar. */
final class AccountChooser {
  static native int nativeChoose(String packageName, String[] labels,
      boolean canAdd, BooleanSupplier isActive);
}
