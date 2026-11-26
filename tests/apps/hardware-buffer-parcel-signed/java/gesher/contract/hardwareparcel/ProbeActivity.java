package gesher.contract.hardwareparcel;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.content.ServiceConnection;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.hardware.HardwareBuffer;
import android.media.Image;
import android.media.ImageReader;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;
import android.util.Log;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class ProbeActivity extends Activity {
    static final String TAG = "HardwareParcelBinder";
    static final String DESCRIPTOR = "gesher.contract.hardwareparcel.Receiver";
    static final int REQUEST = IBinder.FIRST_CALL_TRANSACTION;

    private final ServiceConnection connection = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName name, IBinder binder) {
            new Thread(() -> {
                try {
                    send(binder);
                } catch (Throwable problem) {
                    Log.e(TAG, "FAIL", problem);
                } finally {
                    runOnUiThread(() -> { unbindService(this); finish(); });
                }
            }, "hardware-parcel-sender").start();
        }

        @Override public void onServiceDisconnected(ComponentName name) {}
    };

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        if (!bindService(new Intent(this, Receiver.class), connection, BIND_AUTO_CREATE)) {
            Log.e(TAG, "FAIL service not admitted");
            finish();
        }
    }

    static void require(boolean condition, String detail) {
        if (!condition) throw new AssertionError(detail);
    }

    private static void send(IBinder receiver) throws Exception {
        HandlerThread notifications = new HandlerThread("hardware-parcel-frames");
        notifications.start();
        Parcel data = Parcel.obtain();
        Parcel reply = Parcel.obtain();
        try {
            data.writeInterfaceToken(DESCRIPTOR);
            final long id;
            try (ImageReader reader = new ImageReader.Builder(18, 10).setMaxImages(2)
                    .setImageFormat(PixelFormat.RGBA_8888)
                    .setUsage(HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE).build()) {
                CountDownLatch available = new CountDownLatch(1);
                reader.setOnImageAvailableListener(ignored -> available.countDown(),
                        new Handler(notifications.getLooper()));
                Canvas canvas = reader.getSurface().lockCanvas(null);
                canvas.drawColor(Color.RED);
                canvas.clipRect(0, 0, 9, 10);
                canvas.drawColor(Color.BLUE);
                reader.getSurface().unlockCanvasAndPost(canvas);
                require(available.await(5, TimeUnit.SECONDS), "published actual frame");
                try (Image image = reader.acquireNextImage()) {
                    require(image != null, "acquire frame");
                    try (HardwareBuffer buffer = image.getHardwareBuffer()) {
                        require(buffer != null, "acquire buffer");
                        id = buffer.getId();
                        data.writeInt(42);
                        buffer.writeToParcel(data, 0);
                        data.writeInt(99);
                    }
                }
            }
            require(data.hasFileDescriptors(), "Parcel retains the buffer descriptor");
            require(receiver.transact(REQUEST, data, reply, 0), "remote transaction");
            reply.readException();
            final int sendingPid = Process.myPid();
            final int receivingPid = reply.readInt();
            require(receivingPid > 0 && sendingPid != receivingPid, "two app processes");
            require(reply.readLong() == id, "buffer ID was preserved");
            require(reply.readInt() == Color.BLUE && reply.readInt() == Color.RED, "two true pixel colors");
            require(reply.dataAvail() == 0, "reply boundaries");
            Log.i(TAG, "PASS sender=" + sendingPid + " receiver=" + receivingPid + " id=" + id);
        } finally {
            reply.recycle();
            data.recycle();
            notifications.quitSafely();
            notifications.join(5000);
        }
    }
}
