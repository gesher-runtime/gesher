package com.gesher.server;

/** A main-class string in an extra argument must not acquire the server role. */
public final class ChooserClient {
  interface Action { void run(); }
  static void denied(Action action) {
    try { action.run(); }
    catch (SecurityException expected) { return; }
    throw new AssertionError("Non-server native call accepted");
  }
  public static void main(String[] args) {
    System.loadLibrary("android_runtime");
    System.load(args[0]);
    denied(() -> AccountChooser.nativeChoose("com.gesher.synthetic",
        new String[]{"Synthetic"}, true, () -> false));
    denied(() -> AccountSecretStore.nativeProtect(new byte[]{1}));
    denied(() -> AccountSecretStore.nativeUnprotect(new byte[]{1}));
    System.out.println("AccountChooserNative client role all passed: 3");
  }
}
