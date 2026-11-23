package android.os;
public final class SystemClock {
    public static long uptimeMillis() { return System.nanoTime() / 1_000_000; }
    public static void sleep(long ms) { try { Thread.sleep(ms); } catch (InterruptedException ignored) { } }
}
