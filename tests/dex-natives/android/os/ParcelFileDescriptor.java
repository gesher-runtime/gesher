package android.os;

import java.io.FileDescriptor;

/** Stand-in for the framework class: what the asset natives construct and call. */
public class ParcelFileDescriptor {
  private final FileDescriptor mFd;

  public ParcelFileDescriptor(FileDescriptor fd) {
    mFd = fd;
  }

  public FileDescriptor getFileDescriptor() {
    return mFd;
  }

  public int detachFd() {
    return mFd.getInt$();
  }
}
