// Host class layout and native descriptors from Android 16 r4 MediaCodec.java.
package android.media;
import android.view.Surface;

public final class MediaCodec {
    public static Surface createPersistentInputSurface() {
        return native_createPersistentInputSurface();
    }
    static class PersistentSurface extends Surface {
        PersistentSurface() {}
        private long mPersistentObject;
        @Override public void release() {
            native_releasePersistentInputSurface(this);
            super.release();
        }
    }
    private static native PersistentSurface native_createPersistentInputSurface();
    private static native void native_releasePersistentInputSurface(Surface surface);
}
