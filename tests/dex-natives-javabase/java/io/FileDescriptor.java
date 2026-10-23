package java.io;

/**
 * Compile-time view of libcore's FileDescriptor (patched into java.base for
 * javac only, never dexed): the Android-only accessor MessageQueue calls.
 */
public final class FileDescriptor {
  public FileDescriptor() {}

  public int getInt$() {
    throw new UnsupportedOperationException();
  }
}
