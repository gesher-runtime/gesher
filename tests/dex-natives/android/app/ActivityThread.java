package android.app;

/** Stand-in for the framework class: what MessageQueue's test hooks call. */
public final class ActivityThread {
  private ActivityThread() {}

  public static void throwIfNotInstrumenting() {
    throw new IllegalStateException("Test-only API used without instrumentation");
  }
}
