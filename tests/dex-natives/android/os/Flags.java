package android.os;

/**
 * Stand-in for the aconfig-generated android.os.Flags: the two MessageQueue
 * reads. The concurrent queue is chosen with the system property
 * gesher.mq.concurrent (java -D), so the test can run both implementations
 * CombinedMessageQueue/MessageQueue.java carries.
 */
public final class Flags {
  public static final String FLAG_MAINLINE_VCN_PLATFORM_API = "android.os.mainline_vcn_platform_api";

  private Flags() {}

  public static boolean useConcurrentMessageQueueInApps() {
    return Boolean.getBoolean("gesher.mq.concurrent");
  }

  public static boolean nativeLooperSkipEpollWaitForZeroTimeout() {
    return true;
  }
}
