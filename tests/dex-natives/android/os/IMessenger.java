package android.os;

/** Stand-in for the AIDL interface until libbinder (step 5): local calls only. */
public interface IMessenger {
  void send(Message msg) throws RemoteException;

  abstract class Stub extends Binder implements IMessenger {}
}
