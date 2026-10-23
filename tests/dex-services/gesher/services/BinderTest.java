package gesher.services;

import android.os.Binder;
import android.os.DeadObjectException;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;
import android.os.RemoteException;
import android.os.ServiceManager;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

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

  public static void main(String[] args) throws Exception {
    System.loadLibrary("android_runtime");
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
