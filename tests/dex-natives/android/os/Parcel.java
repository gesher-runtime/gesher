package android.os;

/** Stand-in for the framework class until libbinder's Parcel (step 5): the API Message uses. */
public final class Parcel {
  private Parcel() {}

  private static UnsupportedOperationException unsupported() {
    return new UnsupportedOperationException("Parcel is not ported yet");
  }

  public void writeInt(int v) {
    throw unsupported();
  }

  public void writeLong(long v) {
    throw unsupported();
  }

  public void writeParcelable(Parcelable p, int flags) {
    throw unsupported();
  }

  public void writeBundle(Bundle b) {
    throw unsupported();
  }

  public void writeStrongBinder(Object binder) {
    throw unsupported();
  }

  public int readInt() {
    throw unsupported();
  }

  public long readLong() {
    throw unsupported();
  }

  public <T> T readParcelable(ClassLoader loader, Class<T> clazz) {
    throw unsupported();
  }

  public Bundle readBundle() {
    throw unsupported();
  }
}
