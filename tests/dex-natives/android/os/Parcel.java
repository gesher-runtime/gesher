package android.os;

/**
 * Stand-in for the framework class. Without framework.jar it is the API
 * Message names and is never used; with framework.jar on the boot class path
 * the real class is used, and this is only the API gesher.FrameworkNativesTest
 * compiles against (same signatures).
 */
public final class Parcel {
  private Parcel() {}

  private static UnsupportedOperationException unsupported() {
    return new UnsupportedOperationException("Parcel needs framework.jar");
  }

  public static Parcel obtain() {
    throw unsupported();
  }

  public final void recycle() {
    throw unsupported();
  }

  public final int dataSize() {
    throw unsupported();
  }

  public final int dataPosition() {
    throw unsupported();
  }

  public final void setDataPosition(int pos) {
    throw unsupported();
  }

  public final byte[] marshall() {
    throw unsupported();
  }

  public final void unmarshall(byte[] data, int offset, int length) {
    throw unsupported();
  }

  public final void writeInt(int v) {
    throw unsupported();
  }

  public final void writeLong(long v) {
    throw unsupported();
  }

  public final void writeDouble(double v) {
    throw unsupported();
  }

  public final void writeString(String v) {
    throw unsupported();
  }

  public final void writeByteArray(byte[] v) {
    throw unsupported();
  }

  public final void writeStrongBinder(IBinder binder) {
    throw unsupported();
  }

  public final void writeInterfaceToken(String descriptor) {
    throw unsupported();
  }

  public final void enforceInterface(String descriptor) {
    throw unsupported();
  }

  public final void writeParcelable(Parcelable p, int flags) {
    throw unsupported();
  }

  public final void writeBundle(Bundle b) {
    throw unsupported();
  }

  public final int readInt() {
    throw unsupported();
  }

  public final long readLong() {
    throw unsupported();
  }

  public final double readDouble() {
    throw unsupported();
  }

  public final String readString() {
    throw unsupported();
  }

  public final byte[] createByteArray() {
    throw unsupported();
  }

  public final IBinder readStrongBinder() {
    throw unsupported();
  }

  public <T> T readParcelable(ClassLoader loader, Class<T> clazz) {
    throw unsupported();
  }

  public final Bundle readBundle() {
    throw unsupported();
  }
}
