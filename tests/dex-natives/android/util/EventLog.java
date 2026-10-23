package android.util;

import java.util.Collection;

/** Stand-in for the framework class; the natives match android_util_EventLog.cpp. */
public class EventLog {
  /** register_android_util_EventLog caches this class and its constructor. */
  public static final class Event {
    private final byte[] mBuffer;

    Event(byte[] data) {
      mBuffer = data;
    }

    public byte[] getBytes() {
      return mBuffer.clone();
    }
  }

  public static native int writeEvent(int tag, int value);

  public static native int writeEvent(int tag, long value);

  public static native int writeEvent(int tag, float value);

  public static native int writeEvent(int tag, String value);

  public static native int writeEvent(int tag, Object... values);

  public static native void readEvents(int[] tags, Collection<Event> output);

  public static native void readEventsOnWrapping(int[] tags, long timestamp,
      Collection<Event> output);
}
