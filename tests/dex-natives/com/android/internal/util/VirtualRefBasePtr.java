package com.android.internal.util;

/** Stand-in for the framework class; the natives match
 * com_android_internal_util_VirtualRefBasePtr.cpp. */
public final class VirtualRefBasePtr {
  private long mNativePtr;

  public VirtualRefBasePtr(long ptr) {
    mNativePtr = ptr;
    nIncStrong(ptr);
  }

  public long get() {
    return mNativePtr;
  }

  public void release() {
    if (mNativePtr != 0) {
      nDecStrong(mNativePtr);
      mNativePtr = 0;
    }
  }

  private static native void nIncStrong(long ptr);

  private static native void nDecStrong(long ptr);
}
