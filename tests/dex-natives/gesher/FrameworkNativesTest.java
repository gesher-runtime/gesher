package gesher;

import android.os.Binder;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import android.os.RemoteException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * libandroid_runtime on the real framework.jar (docs/framework-natives.md,
 * steps 3 and 5): run with framework.jar on the boot class path, so every
 * android.* class here is the framework's and the classes in natives.dex are
 * only what this compiles against. Loading the library registers every table
 * on the framework's classes, which checks that each native exists there. Then
 * MessageQueue (a HandlerThread), Parcel (libbinder's, through the natives)
 * and an in-process Binder transaction, with a Binder written to and read back
 * from a Parcel. Prints "all passed" and exits 0 when every check holds.
 */
public final class FrameworkNativesTest {
  private static int fails = 0;

  private static void check(boolean ok, String what) {
    if (!ok) {
      System.out.println("FAIL: " + what);
      fails++;
    }
  }

  /** A local service: replies "echo:" + the request and the calling uid. */
  static final class Echo extends Binder {
    int calls;

    @Override
    protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
        throws RemoteException {
      if (code != 1) return super.onTransact(code, data, reply, flags);
      calls++;
      data.enforceInterface("gesher.Echo");
      reply.writeString("echo:" + data.readString());
      reply.writeInt(Binder.getCallingUid());
      return true;
    }
  }

  public static void main(String[] args) throws Exception {
    System.loadLibrary("android_runtime");
    check(Binder.class.getClassLoader() == Object.class.getClassLoader(),
        "android.os.Binder is framework.jar's");

    // MessageQueue: a HandlerThread runs a delayed post.
    HandlerThread thread = new HandlerThread("gesher-framework");
    thread.start();
    CountDownLatch latch = new CountDownLatch(1);
    long posted = System.nanoTime();
    new Handler(thread.getLooper()).postDelayed(latch::countDown, 50);
    check(latch.await(5, TimeUnit.SECONDS), "HandlerThread ran a delayed post");
    check(System.nanoTime() - posted >= 40_000_000L, "postDelayed waited");
    check(thread.quitSafely(), "quitSafely");
    thread.join(5000);
    check(Looper.getMainLooper() == null || Looper.getMainLooper() != thread.getLooper(),
        "main looper untouched");

    // Parcel: primitives and strings round-trip, also through marshall.
    Parcel p = Parcel.obtain();
    p.writeInt(42);
    p.writeLong(1L << 40);
    p.writeDouble(2.5);
    p.writeString("gesher שלום");
    p.writeByteArray(new byte[] {1, 2, 3});
    p.setDataPosition(0);
    check(p.readInt() == 42, "Parcel int");
    check(p.readLong() == 1L << 40, "Parcel long");
    check(p.readDouble() == 2.5, "Parcel double");
    check("gesher שלום".equals(p.readString()), "Parcel string");
    byte[] bytes = p.createByteArray();
    check(bytes != null && bytes.length == 3 && bytes[2] == 3, "Parcel byte[]");
    byte[] wire = p.marshall();
    Parcel q = Parcel.obtain();
    q.unmarshall(wire, 0, wire.length);
    q.setDataPosition(0);
    check(q.readInt() == 42 && q.readLong() == 1L << 40, "Parcel marshall/unmarshall");
    check(q.dataSize() == p.dataSize(), "unmarshalled size");
    p.recycle();
    q.recycle();

    // Binder: an in-process transaction, and a local Binder through a Parcel.
    Echo echo = new Echo();
    Parcel data = Parcel.obtain();
    Parcel reply = Parcel.obtain();
    data.writeInterfaceToken("gesher.Echo");
    data.writeString("hi");
    check(echo.transact(1, data, reply, 0), "transact returned true");
    reply.setDataPosition(0);
    check("echo:hi".equals(reply.readString()), "transaction reply");
    check(reply.readInt() == Binder.getCallingUid(), "calling uid in-process");
    check(echo.calls == 1, "onTransact ran once");
    long token = Binder.clearCallingIdentity();
    Binder.restoreCallingIdentity(token);
    Parcel holder = Parcel.obtain();
    holder.writeStrongBinder(echo);
    holder.setDataPosition(0);
    IBinder back = holder.readStrongBinder();
    check(back == echo, "a local Binder reads back as itself");
    data.recycle();
    reply.recycle();
    holder.recycle();

    System.out.println(fails == 0 ? "all passed" : fails + " failures");
    System.exit(fails == 0 ? 0 : 1);
  }
}
