package gesher.services;

import android.os.Binder;
import android.os.DeadObjectException;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.SystemProperties;

import java.io.BufferedReader;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Binder between two dalvikvm processes, through the framework's own
 * ServiceManager, Binder, BinderProxy and Parcel over libbinder, servicemanager
 * and binderd (docs/services.md). tests/windows/tracks/50-services.ps1 starts
 * binderd and servicemanager, then
 *
 *   BinderTest service    registers "gesher.binder.test" and joins the
 *                         binder thread pool
 *   BinderTest client     finds it, calls it with a Parcel holding a Binder
 *                         of its own, which the service calls back (a nested
 *                         call into the waiting client) and passes back with
 *                         itself; then links to its death and tells it to
 *                         exit
 */
public final class BinderTest {
  static final String NAME = "gesher.binder.test";
  static final String DESCRIPTOR = "gesher.services.IBinderTest";
  static final int PING = IBinder.FIRST_CALL_TRANSACTION;
  static final int EXIT = IBinder.FIRST_CALL_TRANSACTION + 1;
  static final int CALLBACK = IBinder.FIRST_CALL_TRANSACTION + 2;
  static final int REPORT_STATE = IBinder.FIRST_CALL_TRANSACTION + 3;
  static final int REMOVE_REPORT_CALLBACK = IBinder.FIRST_CALL_TRANSACTION + 4;
  static final String REPORT_PROPERTY = "debug.exec4.binder.report";
  static final AtomicInteger reports = new AtomicInteger();
  static volatile String reportedValue = "unreported";
  static final Runnable propertyCallback = () -> {
    reportedValue = SystemProperties.get(REPORT_PROPERTY);
    reports.incrementAndGet();
  };

  public static void main(String[] args) throws Exception {
    System.loadLibrary("android_runtime");
    // android.os.Build reads the system properties once (its ABI list must not be
    // empty); binder calls initialize it (AppOpsManager -> ActivityThread -> Build).
    loadBuildProps(System.getenv("ANDROID_ROOT") + "/build.prop");
    if (args.length > 0 && args[0].equals("service")) {
      service();
    } else {
      client();
    }
  }

  // --- the service ---------------------------------------------------------------

  static final class TestService extends Binder {
    TestService() {
      super(DESCRIPTOR);
    }

    @Override
    protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
        throws RemoteException {
      switch (code) {
        case REPORT_STATE:
        case REMOVE_REPORT_CALLBACK:
          data.enforceInterface(DESCRIPTOR);
          if (code == REMOVE_REPORT_CALLBACK) {
            SystemProperties.removeChangeCallback(propertyCallback);
          }
          reply.writeNoException();
          reply.writeInt(reports.get());
          reply.writeString(reportedValue);
          reply.writeInt(Process.myPid());
          return true;
        case PING: {
          data.enforceInterface(DESCRIPTOR);
          IBinder callback = data.readStrongBinder();
          String text = data.readString();
          Parcel d = Parcel.obtain();
          Parcel r = Parcel.obtain();
          try {
            d.writeString("from the service, to " + text);
            callback.transact(CALLBACK, d, r, 0);
            reply.writeNoException();
            reply.writeString(r.readString());
            reply.writeStrongBinder(callback);
            reply.writeStrongBinder(this);
            reply.writeInt(Binder.getCallingPid());
            reply.writeInt(Process.myPid());
          } finally {
            d.recycle();
            r.recycle();
          }
          return true;
        }
        case EXIT:
          data.enforceInterface(DESCRIPTOR);
          reply.writeNoException();
          new Thread(() -> {
            sleep(200);
            System.out.println("BinderTest service: exiting");
            System.exit(0);
          }).start();
          return true;
        default:
          return super.onTransact(code, data, reply, flags);
      }
    }
  }

  static void service() {
    SystemProperties.addChangeCallback(propertyCallback);
    ServiceManager.addService(NAME, new TestService());
    System.out.println("BinderTest service: registered " + NAME + " in pid " + Process.myPid());
    Binder.joinThreadPool();
  }

  // --- the client ----------------------------------------------------------------

  static final class Callback extends Binder {
    volatile Thread calledOn;
    volatile int callingPid;

    @Override
    protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
        throws RemoteException {
      if (code != CALLBACK) return super.onTransact(code, data, reply, flags);
      calledOn = Thread.currentThread();
      callingPid = Binder.getCallingPid();
      reply.writeString("callback got: " + data.readString());
      return true;
    }
  }

  static void client() throws Exception {
    // Incoming calls that are not nested in an outgoing one (binderDied)
    // need a binder thread.
    Thread looper = new Thread(Binder::joinThreadPool, "binder looper");
    looper.setDaemon(true);
    looper.start();

    IBinder service = null;
    for (int i = 0; i < 600 && service == null; i++) {
      service = ServiceManager.checkService(NAME);
      if (service == null) sleep(50);
    }
    check(service != null, "ServiceManager.checkService finds the service");
    check(!(service instanceof Binder), "the service is a proxy, in another process");
    check(DESCRIPTOR.equals(service.getInterfaceDescriptor()), "getInterfaceDescriptor");
    check(service.pingBinder(), "pingBinder");

    Callback callback = new Callback();
    Parcel data = Parcel.obtain();
    Parcel reply = Parcel.obtain();
    data.writeInterfaceToken(DESCRIPTOR);
    data.writeStrongBinder(callback);
    data.writeString("the client");
    check(service.transact(PING, data, reply, 0), "transact");
    reply.readException();
    String text = reply.readString();
    IBinder back = reply.readStrongBinder();
    IBinder self = reply.readStrongBinder();
    int callingPid = reply.readInt();
    int servicePid = reply.readInt();
    data.recycle();
    reply.recycle();
    check("callback got: from the service, to the client".equals(text), "reply text: " + text);
    check(back == callback, "the client's own Binder comes back as itself");
    check(self == service, "the service's Binder comes back as the same proxy");
    check(callingPid == Process.myPid(), "Binder.getCallingPid in the service: " + callingPid);
    check(servicePid != Process.myPid(), "the service runs in another process");
    check(callback.calledOn == Thread.currentThread(),
        "the nested call runs on the calling thread: " + callback.calledOn);
    check(callback.callingPid == servicePid, "Binder.getCallingPid in the callback");

    String[] names = ServiceManager.listServices();
    boolean listed = false;
    for (String n : names) listed |= NAME.equals(n);
    check(listed, "listServices names the service");

    propertyReports(service, servicePid);

    CountDownLatch died = new CountDownLatch(1);
    service.linkToDeath(died::countDown, 0);
    data = Parcel.obtain();
    reply = Parcel.obtain();
    data.writeInterfaceToken(DESCRIPTOR);
    check(service.transact(EXIT, data, reply, 0), "transact EXIT");
    reply.readException();
    data.recycle();
    reply.recycle();
    check(died.await(30, TimeUnit.SECONDS), "binderDied after the service exits");
    check(!service.isBinderAlive(), "isBinderAlive is false");
    boolean dead = false;
    try {
      data = Parcel.obtain();
      service.transact(PING, data, null, 0);
    } catch (DeadObjectException e) {
      dead = true;
    }
    check(dead, "a call to the dead service throws DeadObjectException");
    System.out.println("BinderTest all passed");
    System.exit(0);
  }

  static void reportState(IBinder service, int code, int wanted, String value, int servicePid)
      throws RemoteException {
    Parcel data = Parcel.obtain();
    Parcel reply = Parcel.obtain();
    try {
      data.writeInterfaceToken(DESCRIPTOR);
      check(service.transact(code, data, reply, 0), "report state transaction");
      reply.readException();
      int actual = reply.readInt();
      String observed = reply.readString();
      int pid = reply.readInt();
      System.out.println("BinderTest report state: count=" + actual + " value=" + observed
          + " recipient=" + pid + " client=" + Process.myPid());
      check(pid == servicePid && pid != Process.myPid(), "report state is from remote recipient");
      check(actual == wanted && value.equals(observed), "remote report count and observed value");
    } finally {
      data.recycle();
      reply.recycle();
    }
  }

  static void poke(IBinder service) throws RemoteException {
    Parcel data = Parcel.obtain();
    try {
      // SystemPropPoker sends an empty Parcel and ignores the boolean result.
      // JavaBBinder always dispatches SYSPROPS through BBinder::onTransact,
      // even when Java Binder.onTransact returns false for this reserved code.
      boolean handled = service.transact(IBinder.SYSPROPS_TRANSACTION, data, null, 0);
      System.out.println("BinderTest actual SYSPROPS_TRANSACTION handled=" + handled);
    } finally {
      data.recycle();
    }
  }

  static void propertyReports(IBinder service, int servicePid) throws Exception {
    AtomicInteger localReports = new AtomicInteger();
    Runnable localCallback = localReports::incrementAndGet;
    SystemProperties.addChangeCallback(localCallback);
    try {
      reportState(service, REPORT_STATE, 0, "unreported", servicePid);
      SystemProperties.set(REPORT_PROPERTY, "first");
      reportState(service, REPORT_STATE, 0, "unreported", servicePid);
      check(localReports.get() == 0, "ordinary property set does not fabricate a local report");
      SystemProperties.reportSyspropChanged();
      check(localReports.get() == 1, "explicit report stays local");
      reportState(service, REPORT_STATE, 0, "unreported", servicePid);
      poke(service);
      reportState(service, REPORT_STATE, 1, "first", servicePid);
      check(localReports.get() == 1, "remote poke does not report in the client");
      SystemProperties.set(REPORT_PROPERTY, "second");
      reportState(service, REPORT_STATE, 1, "first", servicePid);
      poke(service);
      reportState(service, REPORT_STATE, 2, "second", servicePid);
      reportState(service, REMOVE_REPORT_CALLBACK, 2, "second", servicePid);
      SystemProperties.set(REPORT_PROPERTY, "after-removal");
      poke(service);
      reportState(service, REPORT_STATE, 2, "second", servicePid);
      check(localReports.get() == 1, "remote reports leave client callback untouched");
      System.out.println("BinderTest PASS remote SYSPROPS reports, live values, local-only explicit report, ordinary-set silence and callback removal");
    } finally {
      SystemProperties.removeChangeCallback(localCallback);
    }
  }

  /** $ANDROID_ROOT/build.prop into the system properties, as gesher-app's BuildProps. */
  static void loadBuildProps(String path) throws IOException {
    try (BufferedReader r = new BufferedReader(
        new InputStreamReader(new FileInputStream(path), StandardCharsets.UTF_8))) {
      String line;
      while ((line = r.readLine()) != null) {
        line = line.trim();
        int eq = line.indexOf('=');
        if (line.startsWith("#") || eq <= 0) continue;
        String key = line.substring(0, eq).trim();
        if (key.endsWith("?")) key = key.substring(0, key.length() - 1).trim();
        try {
          SystemProperties.set(key, line.substring(eq + 1).trim());
        } catch (RuntimeException e) {
          // Too long for a property, or read-only and already set: as init, skip it.
        }
      }
    }
  }

  static void sleep(long ms) {
    try {
      Thread.sleep(ms);
    } catch (InterruptedException e) {
      throw new RuntimeException(e);
    }
  }

  static void check(boolean ok, String what) {
    if (!ok) throw new AssertionError(what);
    System.out.println("ok: " + what);
  }
}
