package com.gesher.server;

/** Test-only JNI owner for unchanged DPAPI entry points. */
final class AccountSecretStore {
  static native byte[] nativeProtect(byte[] input);
  static native byte[] nativeUnprotect(byte[] input);
}
