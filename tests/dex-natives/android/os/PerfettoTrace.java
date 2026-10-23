package android.os;

/**
 * Stand-in for the framework class: perfetto is not ported, so the message
 * queue category is never enabled (compat/framework/tracing_perfetto_stub.cc).
 */
public final class PerfettoTrace {
  public static final Category MQ_CATEGORY = new Category("mq");
  public static final com.android.internal.dev.perfetto.sdk.PerfettoTrace.Category MQ_CATEGORY_V3 =
      new com.android.internal.dev.perfetto.sdk.PerfettoTrace.Category("mq");
  public static final boolean IS_USE_SDK_TRACING_API_V3 = false;

  private PerfettoTrace() {}

  public static boolean isMQCategoryEnabled() {
    return false;
  }

  public static long getFlowId() {
    return 0;
  }

  /** The event builder; nothing is ever emitted. */
  public static final class Builder {
    public Builder beginProto() { return this; }
    public Builder endProto() { return this; }
    public Builder beginNested(long fieldId) { return this; }
    public Builder endNested() { return this; }
    public Builder addField(long fieldId, long value) { return this; }
    public Builder addField(long fieldId, double value) { return this; }
    public Builder addField(long fieldId, String value) { return this; }
    public Builder setFlow(long id) { return this; }
    public Builder setTerminatingFlow(long id) { return this; }
    public Builder usingThreadCounterTrack(long tid, String name) { return this; }
    public void emit() {}
  }

  /** A trace category, never enabled. */
  public static final class Category {
    public Category(String name) {}
    public boolean isEnabled() { return false; }
  }

  public static Builder begin(Category category, String name) { return new Builder(); }
  public static Builder end(Category category) { return new Builder(); }
  public static Builder instant(Category category, String name) { return new Builder(); }
  public static Builder counter(Category category, long value) { return new Builder(); }
}
