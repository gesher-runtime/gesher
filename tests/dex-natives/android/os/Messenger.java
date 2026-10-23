package android.os;

/** Stand-in for the framework class: a local reference to a Handler. */
public final class Messenger {
  private final IMessenger mTarget;

  public Messenger(Handler target) {
    mTarget = target.getIMessenger();
  }

  public void send(Message message) throws RemoteException {
    mTarget.send(message);
  }

  public static void writeMessengerOrNullToParcel(Messenger messenger, Parcel out) {
    throw new UnsupportedOperationException("Parcel needs framework.jar");
  }

  public static Messenger readMessengerOrNullFromParcel(Parcel in) {
    throw new UnsupportedOperationException("Parcel is not ported yet");
  }
}
