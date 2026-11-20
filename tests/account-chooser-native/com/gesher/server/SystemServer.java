package com.gesher.server;

import java.util.Arrays;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BooleanSupplier;

/** Isolated fixture main exercises the existing actual-main-class role guard.
 * It loads no app jar, accounts, preferences or Google packages. */
public final class SystemServer {
  interface Action { void run(); }
  private static int checks;
  static void check(boolean value) {
    if (!value) throw new AssertionError("Synthetic chooser contract failed");
    checks++;
  }
  private static void invalid(Action action) {
    try { action.run(); }
    catch (IllegalArgumentException expected) { checks++; return; }
    throw new AssertionError("Invalid chooser input accepted");
  }

  private static int choose(boolean intent, String pkg, String[] labels, boolean add, BooleanSupplier active) {
    boolean[] enabled = new boolean[labels == null ? 0 : labels.length];
    Arrays.fill(enabled, true);
    return intent ? IntentChooser.nativeChoose(pkg, "Synthetic intent choice", labels, enabled, active)
        : AccountChooser.nativeChoose(pkg, labels, add, active);
  }
  public static void main(String[] args) {
    System.loadLibrary("android_runtime");
    System.load(args[0]);
    boolean intent = args[1].startsWith("intent-");
    String mode = intent ? args[1].substring(7) : args[1];
    if (intent) check(IntentChooser.nativeAvailable());
    String pkg = "com.gesher.synthetic";
    String[] labels = {"Synthetic Alpha", "Synthetic \u05d0\u05d1\u05d2 \uD83D\uDE00"};
    Thread caller = Thread.currentThread();
    AtomicInteger calls = new AtomicInteger();
    BooleanSupplier inactive = () -> {
      check(Thread.currentThread() == caller);
      calls.incrementAndGet();
      return false;
    };
    if (mode.equals("inputs")) {
      if (intent) {
        invalid(() -> IntentChooser.nativeChoose(pkg, null, labels, new boolean[2], inactive));
        invalid(() -> IntentChooser.nativeChoose(pkg, "Choice", labels, null, inactive));
        invalid(() -> IntentChooser.nativeChoose(pkg, "Choice", labels, new boolean[1], inactive));
      }
      byte[] plain = {3, 1, 4, 1, 5, 9};
      byte[] sealed = AccountSecretStore.nativeProtect(plain);
      check(sealed != null && !Arrays.equals(plain, sealed));
      check(Arrays.equals(plain, AccountSecretStore.nativeUnprotect(sealed)));
      Arrays.fill(sealed, (byte) 0);
      invalid(() -> choose(intent, null, labels, true, inactive));
      invalid(() -> choose(intent, "", labels, true, inactive));
      invalid(() -> choose(intent, "x".repeat(1025), labels, true, inactive));
      invalid(() -> choose(intent, "a\0b", labels, true, inactive));
      invalid(() -> choose(intent, pkg, null, true, inactive));
      invalid(() -> choose(intent, pkg, new String[intent ? 257 : 65], true, inactive));
      invalid(() -> choose(intent, pkg, new String[]{null}, true, inactive));
      invalid(() -> choose(intent, pkg, new String[]{""}, true, inactive));
      invalid(() -> choose(intent, pkg, new String[]{"x".repeat(2049)}, true, inactive));
      invalid(() -> choose(intent, pkg, new String[]{"a\0b"}, true, inactive));
      invalid(() -> choose(intent, pkg, labels, true, null));
      check(calls.get() == 0);
      check(choose(intent, pkg, labels, true, inactive) == -1);
      check(calls.get() == 1);
      String[] maximum = new String[intent ? 256 : 64];
      Arrays.fill(maximum, "x".repeat(2048));
      check(choose(intent, "x".repeat(1024), maximum, true, inactive) == -1);
      check(calls.get() == 2);
      check(choose(intent, pkg, labels, false, () -> { throw new RuntimeException("synthetic"); }) == -1);
      check(choose(intent, pkg, new String[0], false, () -> true) == -1);
      AtomicInteger initCalls = new AtomicInteger();
      check(choose(intent, pkg, labels, true, () -> initCalls.incrementAndGet() == 1) == -1);
      check(initCalls.get() == 2);
    } else {
      BooleanSupplier active = () -> {
        check(Thread.currentThread() == caller);
        int n = calls.incrementAndGet();
        if (mode.equals("timer-cancel") && n >= 3) return false;
        if (mode.equals("timer-exception") && n >= 3) throw new RuntimeException("synthetic");
        return true;
      };
      boolean add = mode.equals("add");
      int selected = intent && mode.equals("suspended")
          ? IntentChooser.nativeChoose(pkg, "Synthetic suspended choice", labels, new boolean[]{true, false}, active)
          : choose(intent, pkg, add ? new String[0] : labels, add, active);
      check(selected == (mode.equals("select") ? 1 : add ? -2 : -1));
      check(calls.get() >= (mode.startsWith("timer-") ? 3 : 2));
    }
    System.out.println("AccountChooserNative all passed mode=" + args[1] + " checks=" + checks);
  }
}
