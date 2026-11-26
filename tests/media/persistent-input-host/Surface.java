// Explicit host-only Java/native Surface boundary. Android's real Surface and
// Parcel/EGL implementations are exercised by recordapp's Windows acceptance.
package android.view;

public class Surface {
    final Object mLock = new Object();
    long mNativeObject;
    public static int failTransfer;
    public Surface() {}
    public Surface(long object) { mNativeObject = object; }
    public void transferFrom(Surface other) {
        if (failTransfer == 1) throw new IllegalStateException("before transfer");
        if (other == null) throw new IllegalArgumentException("other is null");
        if (other != this) {
            final long transferred;
            synchronized (other.mLock) {
                transferred = other.mNativeObject;
                other.mNativeObject = 0;
            }
            synchronized (mLock) {
                if (mNativeObject != 0) nativeRelease(mNativeObject);
                mNativeObject = transferred;
            }
        }
        if (failTransfer == 2) throw new IllegalStateException("after transfer");
    }
    public void release() {
        synchronized (mLock) {
            if (mNativeObject != 0) {
                nativeRelease(mNativeObject);
                mNativeObject = 0;
            }
        }
    }
    public boolean isValid() { synchronized (mLock) { return mNativeObject != 0; } }
    @Override protected void finalize() throws Throwable {
        try { release(); } finally { super.finalize(); }
    }
    private static native void nativeRelease(long object);
}
