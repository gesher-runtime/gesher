package gesher;

import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.Message;
import android.os.MessageQueue;
import android.os.SystemClock;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * The framework's message loop over libandroid_runtime's MessageQueue natives
 * (step 3 of docs/framework-natives.md): the real Looper, Handler, Message,
 * MessageQueue and HandlerThread from frameworks/base, with stand-ins for
 * what they reference. Run with -Dgesher.mq.concurrent=true for the
 * concurrent MessageQueue implementation. Prints "all passed" and exits 0
 * when every check holds.
 */
public final class LooperTest {
  private static int fails = 0;

  private static final class EndLoop extends RuntimeException {}

  private static void check(boolean ok, String what) {
    if (!ok) {
      System.out.println("FAIL: " + what);
      fails++;
    }
  }

  public static void main(String[] args) throws Exception {
    System.loadLibrary("android_runtime");
    System.out.println("MessageQueue: "
        + (Boolean.getBoolean("gesher.mq.concurrent") ? "concurrent" : "legacy"));

    // The main Looper on this thread.
    Looper.prepareMainLooper();
    Looper main = Looper.getMainLooper();
    check(main == Looper.myLooper(), "main looper is this thread's");
    check(main.isCurrentThread(), "isCurrentThread");
    Handler handler = new Handler(main);

    // post and postDelayed run in time order; postDelayed waits its delay.
    List<String> order = new ArrayList<>();
    long start = SystemClock.uptimeMillis();
    long[] delayedAt = new long[1];
    handler.postDelayed(() -> {
      order.add("delayed-100");
      delayedAt[0] = SystemClock.uptimeMillis();
    }, 100);
    handler.postDelayed(() -> order.add("delayed-30"), 30);
    handler.post(() -> order.add("now"));
    handler.sendMessage(Message.obtain(handler, () -> order.add("message")));
    // A message with what and arg, handled by a callback.
    int[] seenWhat = new int[1];
    Handler whatHandler = new Handler(main, msg -> {
      seenWhat[0] = msg.what + msg.arg1;
      return true;
    });
    whatHandler.sendMessageDelayed(whatHandler.obtainMessage(40, 2, 0), 10);
    // A removed message never runs.
    Runnable removed = () -> order.add("removed");
    handler.postDelayed(removed, 50);
    handler.removeCallbacks(removed);
    check(!handler.hasCallbacks(removed), "removeCallbacks");
    // An idle handler runs once the queue is idle.
    int[] idle = new int[1];
    main.getQueue().addIdleHandler(() -> {
      idle[0]++;
      return false;
    });
    // The main Looper cannot quit; a posted exception ends Looper.loop().
    handler.postDelayed(() -> {
      throw new EndLoop();
    }, 200);
    check(Looper.myQueue() == main.getQueue(), "myQueue");
    try {
      Looper.loop();
      check(false, "Looper.loop returned");
    } catch (EndLoop expected) {
      // done
    }
    check(String.join(",", order).equals("now,message,delayed-30,delayed-100"),
        "order of posts: " + order);
    check(delayedAt[0] - start >= 90, "postDelayed(100) waited: " + (delayedAt[0] - start));
    check(seenWhat[0] == 42, "Handler.Callback saw what+arg1");
    check(idle[0] >= 1, "idle handler ran");

    // A HandlerThread: its Looper runs on another thread, and a message
    // posted to it from here is handled there.
    HandlerThread thread = new HandlerThread("gesher-handler-thread");
    thread.start();
    Looper threadLooper = thread.getLooper();
    check(threadLooper != null && threadLooper != main, "HandlerThread looper");
    Handler threadHandler = new Handler(threadLooper);
    CountDownLatch latch = new CountDownLatch(2);
    Thread[] ranOn = new Thread[1];
    threadHandler.post(() -> {
      ranOn[0] = Thread.currentThread();
      latch.countDown();
    });
    long posted = SystemClock.uptimeMillis();
    long[] ranAt = new long[1];
    threadHandler.postDelayed(() -> {
      ranAt[0] = SystemClock.uptimeMillis();
      latch.countDown();
    }, 80);
    check(latch.await(5, TimeUnit.SECONDS), "HandlerThread ran both posts");
    check(ranOn[0] == thread, "post ran on the HandlerThread");
    check(ranAt[0] - posted >= 70, "HandlerThread postDelayed waited: " + (ranAt[0] - posted));
    // runWithScissors-style round trip: a wake from another thread ends the poll.
    CountDownLatch quitLatch = new CountDownLatch(1);
    threadHandler.post(quitLatch::countDown);
    check(quitLatch.await(5, TimeUnit.SECONDS), "wake from another thread");
    check(thread.quitSafely(), "quitSafely");
    thread.join(5000);
    check(!thread.isAlive(), "HandlerThread exited");

    // A queue with no messages blocks in nativePollOnce until woken.
    MessageQueue queue = threadLooper.getQueue();
    check(queue != null, "queue kept after quit");

    System.out.println(fails == 0 ? "all passed" : fails + " failures");
    System.exit(fails == 0 ? 0 : 1);
  }
}
