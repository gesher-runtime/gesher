package android.content.res;

import android.os.ParcelFileDescriptor;

/** Stand-in for the framework class: the fields ApkAssets' natives read. */
public class AssetFileDescriptor {
  private final ParcelFileDescriptor mFd;
  private final long mStartOffset;
  private final long mLength;

  public AssetFileDescriptor(ParcelFileDescriptor fd, long startOffset, long length) {
    mFd = fd;
    mStartOffset = startOffset;
    mLength = length;
  }
}
