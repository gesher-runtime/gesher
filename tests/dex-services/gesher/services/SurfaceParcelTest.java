/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.SurfaceTexture;
import android.media.Image;
import android.media.ImageReader;
import android.os.Binder;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;
import android.os.ServiceManager;
import android.os.SystemProperties;
import android.system.Os;
import android.view.Surface;

import java.io.File;
import java.io.FileInputStream;
import java.lang.reflect.Field;
import java.nio.ByteBuffer;
import java.util.HashMap;
import java.util.Map;
import java.util.Properties;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;

/**
 * android.view.Surface in Parcels (compat/framework/surface_parcel.cc, docs/graphics.md)
 * through the framework's Surface, SurfaceTexture and ImageReader:
 *
 * <ul>
 * <li>no argument: one process. A Surface read back where its consumer is, is the
 *     consumer's own native surface (its frames go straight to the consumer), and
 *     readFromParcel of the same surface keeps it; a released Surface travels as none.
 * <li>{@code service NAME}: a producer process (a browser's GPU process). Its Binder reads
 *     Surfaces from parcels and draws into them with Canvas when asked.
 * <li>{@code client NAME}: the consumer process (the browser, which hands the GPU process
 *     its SurfaceView's Surface). An ImageReader's frames from the producer must arrive
 *     with their pixels and the producer's time stamps, through one memfd the producer
 *     reuses, leaving no descriptor here. Parceled back, the Surface is the consumer's own.
 *     A SurfaceTexture's Surface read into the producer's Surface releases the earlier
 *     one; the SurfaceTexture's new default size reaches the producer with its next
 *     frame; released, its producer's lockCanvas then fails as on an abandoned
 *     BufferQueue. A producer's Surface released takes its memfd with it.
 * </ul>
 */
public final class SurfaceParcelTest {
    private static final String DESCRIPTOR = "gesher.services.ISurfaceParcelTest";
    private static final int ATTACH = IBinder.FIRST_CALL_TRANSACTION;
    private static final int DRAW = ATTACH + 1;
    private static final int RETURN = ATTACH + 2;
    private static final int RELEASE = ATTACH + 3;
    private static final int MEMFDS = ATTACH + 4;
    private static final int EXIT = ATTACH + 5;

    private static void require(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
    }

    private static void say(String line) {
        System.out.println("SurfaceParcelTest: " + line);
        System.out.flush();
    }

    /** Surface.mNativeObject: the native surface a Java Surface holds. */
    private static long nativeObject(Surface surface) {
        try {
            Field field = Surface.class.getDeclaredField("mNativeObject");
            field.setAccessible(true);
            return field.getLong(surface);
        } catch (ReflectiveOperationException e) {
            throw new AssertionError("Surface.mNativeObject", e);
        }
    }

    /** This process's descriptors open on a memfd ("/memfd:" on Linux; gesher-base's mfd*.tmp). */
    private static int memfds() {
        String[] fds = new File("/proc/self/fd").list();
        require(fds != null, "/proc/self/fd cannot be listed");
        int count = 0;
        for (String fd : fds) {
            String target;
            try {
                target = Os.readlink("/proc/self/fd/" + fd);
            } catch (Exception gone) {
                continue;  // the listing's own descriptor
            }
            String name = target.substring(target.lastIndexOf('/') + 1);
            if (target.startsWith("/memfd:") || (name.startsWith("mfd") && name.endsWith(".tmp"))) {
                count++;
            }
        }
        return count;
    }

    private static Surface roundTrip(Surface surface) {
        Parcel parcel = Parcel.obtain();
        try {
            surface.writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            return Surface.CREATOR.createFromParcel(parcel);
        } finally {
            parcel.recycle();
        }
    }

    // --- one process -------------------------------------------------------------------

    private static void local() throws Exception {
        HandlerThread thread = new HandlerThread("surface-parcel-frames");
        thread.start();
        SurfaceTexture texture = new SurfaceTexture(false);
        texture.setDefaultBufferSize(64, 32);
        Semaphore frames = new Semaphore(0);
        texture.setOnFrameAvailableListener(t -> frames.release(), new Handler(thread.getLooper()));
        Surface surface = new Surface(texture);
        Surface read = roundTrip(surface);
        require(read != null && read.isValid(), "a SurfaceTexture's Surface read back is not valid");
        require(nativeObject(read) == nativeObject(surface),
                "a Surface read back in its consumer's process is not the consumer's own");
        Parcel parcel = Parcel.obtain();
        try {
            surface.writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            long before = nativeObject(read);
            read.readFromParcel(parcel);
            require(nativeObject(read) == before, "readFromParcel of the same surface replaced it");
        } finally {
            parcel.recycle();
        }
        Canvas canvas = read.lockCanvas(null);
        String size = canvas.getWidth() + "x" + canvas.getHeight();
        canvas.drawColor(Color.RED);
        read.unlockCanvasAndPost(canvas);
        require(frames.tryAcquire(5, TimeUnit.SECONDS), "a frame through the Surface read back did not reach the SurfaceTexture");
        require(size.equals("64x32"), "the Surface read back draws " + size + ", not the consumer's 64x32");
        // The reader's reference was its own: the consumer's Surface still draws.
        read.release();
        canvas = surface.lockCanvas(null);
        canvas.drawColor(Color.BLUE);
        surface.unlockCanvasAndPost(canvas);
        require(frames.tryAcquire(5, TimeUnit.SECONDS), "the consumer's Surface lost its surface with the copy's release");
        say("a SurfaceTexture's Surface read back in its own process is the same native surface"
                + " (64x32, its frames reach the SurfaceTexture; readFromParcel keeps it)");

        try (ImageReader reader = ImageReader.newInstance(16, 8, PixelFormat.RGBA_8888, 2)) {
            Surface readerCopy = roundTrip(reader.getSurface());
            require(nativeObject(readerCopy) == nativeObject(reader.getSurface()),
                    "an ImageReader's Surface read back is not the reader's own");
            readerCopy.release();
        }
        say("an ImageReader's Surface read back in its own process is the same native surface");

        Surface released = new Surface(texture);
        released.release();
        Surface none = roundTrip(released);
        require(none != null && !none.isValid(), "a released Surface did not travel as no surface");
        // Read into a valid Surface, no surface leaves it invalid.
        Surface other = new Surface(texture);
        parcel = Parcel.obtain();
        try {
            released.writeToParcel(parcel, 0);
            parcel.setDataPosition(0);
            other.readFromParcel(parcel);
        } finally {
            parcel.recycle();
        }
        require(!other.isValid(), "a Surface that read no surface is still valid");
        say("a released Surface travels as no surface (isValid false, also read into a valid Surface)");
        surface.release();
        texture.release();
        thread.quitSafely();
        System.out.println("SurfaceParcelTest all passed");
    }

    // --- the producer process ----------------------------------------------------------

    private static final class Producer extends Binder {
        private final Map<Integer, Surface> mSurfaces = new HashMap<>();

        Producer() {
            super(DESCRIPTOR);
        }

        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
            data.enforceInterface(DESCRIPTOR);
            switch (code) {
                case ATTACH: {
                    int id = data.readInt();
                    Surface surface = mSurfaces.get(id);
                    boolean kept = false;
                    if (surface == null) {
                        surface = Surface.CREATOR.createFromParcel(data);
                        mSurfaces.put(id, surface);
                    } else {
                        long before = nativeObject(surface);
                        surface.readFromParcel(data);
                        kept = nativeObject(surface) == before;
                    }
                    reply.writeNoException();
                    reply.writeInt(Process.myPid());
                    reply.writeBoolean(surface.isValid());
                    reply.writeBoolean(kept);
                    return true;
                }
                case DRAW: {
                    Surface surface = mSurfaces.get(data.readInt());
                    int left = data.readInt(), right = data.readInt();
                    reply.writeNoException();
                    Canvas canvas;
                    try {
                        canvas = surface.lockCanvas(null);
                    } catch (IllegalArgumentException | Surface.OutOfResourcesException e) {
                        reply.writeString(e.getClass().getName());
                        return true;
                    }
                    int width = canvas.getWidth(), height = canvas.getHeight();
                    Paint paint = new Paint();
                    paint.setColor(left);
                    canvas.drawRect(0, 0, width / 2f, height, paint);
                    paint.setColor(right);
                    canvas.drawRect(width / 2f, 0, width, height, paint);
                    surface.unlockCanvasAndPost(canvas);
                    reply.writeString(null);
                    reply.writeInt(width);
                    reply.writeInt(height);
                    return true;
                }
                case RETURN:
                    reply.writeNoException();
                    mSurfaces.get(data.readInt()).writeToParcel(reply, 0);
                    return true;
                case RELEASE:
                    mSurfaces.remove(data.readInt()).release();
                    reply.writeNoException();
                    return true;
                case MEMFDS:
                    reply.writeNoException();
                    reply.writeInt(memfds());
                    return true;
                case EXIT:
                    reply.writeNoException();
                    new Thread(() -> {
                        try {
                            Thread.sleep(200);
                        } catch (InterruptedException ignored) {
                        }
                        System.exit(0);
                    }, "surface-fixture-exit").start();
                    return true;
                default:
                    return false;
            }
        }
    }

    private static void service(String name) {
        ServiceManager.addService(name, new Producer());
        say("producer Binder service ready");
        Binder.joinThreadPool();
    }

    // --- the consumer process ----------------------------------------------------------

    private static final class Attached {
        int pid;
        boolean valid, kept;
    }

    private static final class Drawn {
        String failure;
        int width, height;

        boolean is(int w, int h) {
            return failure == null && width == w && height == h;
        }

        @Override
        public String toString() {
            return failure != null ? failure : width + "x" + height;
        }
    }

    private static Parcel request() {
        Parcel request = Parcel.obtain();
        request.writeInterfaceToken(DESCRIPTOR);
        return request;
    }

    private static Parcel call(IBinder producer, int code, Parcel request) throws Exception {
        Parcel response = Parcel.obtain();
        try {
            require(producer.transact(code, request, response, 0), "transaction " + code + " failed");
            response.readException();
            return response;
        } catch (Exception | Error e) {
            response.recycle();
            throw e;
        } finally {
            request.recycle();
        }
    }

    private static Attached attach(IBinder producer, int id, Surface surface) throws Exception {
        Parcel request = request();
        request.writeInt(id);
        surface.writeToParcel(request, 0);
        Parcel response = call(producer, ATTACH, request);
        try {
            Attached attached = new Attached();
            attached.pid = response.readInt();
            attached.valid = response.readBoolean();
            attached.kept = response.readBoolean();
            return attached;
        } finally {
            response.recycle();
        }
    }

    private static Drawn draw(IBinder producer, int id, int left, int right) throws Exception {
        Parcel request = request();
        request.writeInt(id);
        request.writeInt(left);
        request.writeInt(right);
        Parcel response = call(producer, DRAW, request);
        try {
            Drawn drawn = new Drawn();
            drawn.failure = response.readString();
            if (drawn.failure == null) {
                drawn.width = response.readInt();
                drawn.height = response.readInt();
            }
            return drawn;
        } finally {
            response.recycle();
        }
    }

    private static Surface returned(IBinder producer, int id) throws Exception {
        Parcel request = request();
        request.writeInt(id);
        Parcel response = call(producer, RETURN, request);
        try {
            return Surface.CREATOR.createFromParcel(response);
        } finally {
            response.recycle();
        }
    }

    private static void release(IBinder producer, int id) throws Exception {
        Parcel request = request();
        request.writeInt(id);
        call(producer, RELEASE, request).recycle();
    }

    private static int producerMemfds(IBinder producer) throws Exception {
        Parcel response = call(producer, MEMFDS, request());
        try {
            return response.readInt();
        } finally {
            response.recycle();
        }
    }

    /** Pixels of an RGBA_8888 image that are not `left` in its left half and `right` in its right. */
    private static int differing(Image image, int left, int right) {
        Image.Plane plane = image.getPlanes()[0];
        ByteBuffer pixels = plane.getBuffer();
        int width = image.getWidth(), height = image.getHeight();
        int wrong = 0;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int at = y * plane.getRowStride() + x * plane.getPixelStride();
                int color = Color.argb(pixels.get(at + 3) & 0xff, pixels.get(at) & 0xff,
                        pixels.get(at + 1) & 0xff, pixels.get(at + 2) & 0xff);
                if (color != (x < width / 2 ? left : right)) wrong++;
            }
        }
        return wrong;
    }

    private static void client(String name) throws Exception {
        IBinder producer = null;
        for (int i = 0; i < 200 && producer == null; i++) {
            producer = ServiceManager.checkService(name);
            if (producer == null) Thread.sleep(50);
        }
        require(producer != null && !(producer instanceof Binder), "the producer is not a remote Binder");
        try {
            consume(producer);
        } finally {
            try {
                call(producer, EXIT, request()).recycle();
            } catch (Exception | Error gone) {
                // The producer is gone already.
            }
        }
        System.out.println("SurfaceParcelTest all passed");
    }

    private static void consume(IBinder producer) throws Exception {
        // An ImageReader's Surface drawn in the other process.
        ImageReader reader = ImageReader.newInstance(64, 32, PixelFormat.RGBA_8888, 2);
        Surface readerSurface = reader.getSurface();
        Attached attached = attach(producer, 1, readerSurface);
        require(attached.pid != Process.myPid(), "the producer runs in this process");
        require(attached.valid, "the producer's Surface is not valid");
        int baseline = producerMemfds(producer);
        int own = memfds();
        long before = System.nanoTime();
        Drawn drawn = draw(producer, 1, Color.RED, Color.BLUE);
        long after = System.nanoTime();
        require(drawn.is(64, 32), "the producer draws " + drawn + ", not the consumer's 64x32");
        Image image = reader.acquireLatestImage();
        require(image != null, "the producer's frame did not reach the ImageReader");
        int wrong;
        long timestamp;
        try {
            require(image.getWidth() == 64 && image.getHeight() == 32, "the image is not 64x32");
            wrong = differing(image, Color.RED, Color.BLUE);
            timestamp = image.getTimestamp();
        } finally {
            image.close();
        }
        require(wrong == 0, wrong + " of 2048 pixels differ from the producer's frame");
        require(timestamp >= before && timestamp <= after, "the frame's time stamp " + timestamp
                + " is not the producer's (between " + before + " and " + after + ")");
        say("an ImageReader's Surface drawn by process " + attached.pid + ": 64x32, 0 pixels differ,"
                + " the producer's time stamp");

        for (int i = 0; i < 5; i++) {
            drawn = draw(producer, 1, i % 2 == 0 ? Color.GREEN : Color.WHITE, Color.BLACK);
            require(drawn.is(64, 32), "frame " + (i + 2) + " drawn " + drawn);
            image = reader.acquireLatestImage();
            require(image != null, "frame " + (i + 2) + " did not reach the ImageReader");
            try {
                wrong = differing(image, i % 2 == 0 ? Color.GREEN : Color.WHITE, Color.BLACK);
            } finally {
                image.close();
            }
            require(wrong == 0, "frame " + (i + 2) + ": " + wrong + " pixels differ");
        }
        int producerAfter = producerMemfds(producer);
        require(producerAfter == baseline + 1, "the producer has " + (producerAfter - baseline)
                + " more memfds after six frames, not the one it reuses");
        require(memfds() == own, "the consumer kept " + (memfds() - own) + " frame descriptors");
        say("six frames through one memfd the producer reuses; the consumer keeps no descriptor");

        attached = attach(producer, 1, readerSurface);
        require(attached.kept, "readFromParcel of the same consumer replaced the producer's Surface");
        Surface back = returned(producer, 1);
        require(nativeObject(back) == nativeObject(readerSurface),
                "the producer's Surface parceled back is not the consumer's own");
        back.release();
        say("parceled again it stays the same producer; parceled back, the consumer's own Surface");

        // A SurfaceTexture's Surface read into the producer's Surface.
        HandlerThread thread = new HandlerThread("surface-parcel-frames");
        thread.start();
        SurfaceTexture texture = new SurfaceTexture(false);
        texture.setDefaultBufferSize(64, 32);
        Semaphore frames = new Semaphore(0);
        texture.setOnFrameAvailableListener(t -> frames.release(), new Handler(thread.getLooper()));
        Surface textureSurface = new Surface(texture);
        attached = attach(producer, 1, textureSurface);
        require(!attached.kept && attached.valid, "readFromParcel of another consumer kept the earlier one");
        int replaced = producerMemfds(producer);
        require(replaced == baseline, "the replaced Surface's memfd is still open: it was not released");
        drawn = draw(producer, 1, Color.RED, Color.GREEN);
        require(drawn.is(64, 32), "the producer draws " + drawn + " into the SurfaceTexture");
        require(frames.tryAcquire(5, TimeUnit.SECONDS), "no onFrameAvailable for the producer's frame");
        reader.close();
        say("a SurfaceTexture's Surface read into the producer's releases the earlier one; its frames"
                + " reach the SurfaceTexture");

        texture.setDefaultBufferSize(48, 24);
        Drawn next = draw(producer, 1, Color.RED, Color.GREEN);
        Drawn then = draw(producer, 1, Color.RED, Color.GREEN);
        require(frames.tryAcquire(2, 5, TimeUnit.SECONDS), "no onFrameAvailable after the resize");
        require((next.is(64, 32) || next.is(48, 24)) && then.is(48, 24),
                "after setDefaultBufferSize(48, 24) the producer draws " + next + " then " + then);
        say("SurfaceTexture.setDefaultBufferSize(48, 24) reaches the producer with its next frame: "
                + next + " then " + then);

        texture.release();
        Drawn failed = draw(producer, 1, Color.RED, Color.GREEN);
        if (failed.failure == null) failed = draw(producer, 1, Color.RED, Color.GREEN);
        require("java.lang.IllegalArgumentException".equals(failed.failure),
                "the producer of a released SurfaceTexture still draws: " + failed);
        int abandoned = producerMemfds(producer);
        require(abandoned == baseline, "the abandoned producer's memfd is still open");
        say("the SurfaceTexture released, the producer's lockCanvas throws IllegalArgumentException"
                + " and its memfd is closed");

        try (ImageReader second = ImageReader.newInstance(32, 16, PixelFormat.RGBA_8888, 2)) {
            attached = attach(producer, 2, second.getSurface());
            require(attached.valid, "a second producer Surface is not valid");
            drawn = draw(producer, 2, Color.BLUE, Color.YELLOW);
            require(drawn.is(32, 16), "the second producer draws " + drawn);
            require(producerMemfds(producer) == baseline + 1, "the second producer has no memfd");
            release(producer, 2);
            require(producerMemfds(producer) == baseline, "a released producer Surface kept its memfd");
        }
        say("a producer's Surface released takes its memfd with it");
        textureSurface.release();
        thread.quitSafely();
    }

    public static void main(String[] args) throws Exception {
        System.loadLibrary("android_runtime");
        Properties properties = new Properties();
        try (FileInputStream input = new FileInputStream(System.getenv("ANDROID_ROOT") + "/build.prop")) {
            properties.load(input);
        }
        for (String key : properties.stringPropertyNames()) {
            try {
                SystemProperties.set(key.endsWith("?") ? key.substring(0, key.length() - 1) : key,
                        properties.getProperty(key));
            } catch (RuntimeException alreadyInitialized) {
                // Read-only properties retain runtime init values.
            }
        }
        try {
            if (args.length == 0) local();
            else if (args[0].equals("service")) service(args[1]);
            else if (args[0].equals("client")) client(args[1]);
            else throw new IllegalArgumentException(args[0]);
        } catch (Throwable failure) {
            // A frame thread must not keep a failed run alive.
            failure.printStackTrace();
            System.exit(1);
        }
        System.exit(0);
    }
}
