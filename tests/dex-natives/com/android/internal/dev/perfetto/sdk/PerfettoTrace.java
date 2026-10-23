package com.android.internal.dev.perfetto.sdk;

/** Stand-in for the perfetto SDK class: nothing is ever traced. */
public final class PerfettoTrace {
  private PerfettoTrace() {}

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
