package android.os;

/** Stand-in for the framework interface (compile-time API for tests/dex-natives). */
public interface IBinder {
  boolean transact(int code, Parcel data, Parcel reply, int flags) throws RemoteException;
}
