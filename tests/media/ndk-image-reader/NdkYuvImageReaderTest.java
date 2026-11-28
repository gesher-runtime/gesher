// Copyright 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
package gesher;

import android.media.MediaCodec;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.view.Surface;
import java.io.FileInputStream;

/** Decode two real AVC frames into an AImageReader and verify their native planes. */
public final class NdkYuvImageReaderTest {
    private static final long FIRST_TIMESTAMP_NS = 123_456_789L;
    private static final long SECOND_TIMESTAMP_NS = 678_901_234L;

    private static native long createReader();
    private static native Surface getSurface(long reader);
    private static native long checkImage(long reader, long timestamp, int color);
    private static native void checkRetainedImage(long image);
    private static native void deleteImage(long image);
    private static native void deleteReader(long reader);

    public static void main(String[] args) throws Exception {
        if (args.length != 2) throw new IllegalArgumentException("JNI library and input MP4");
        System.load(args[0]);
        long reader = createReader();
        Surface surface = getSurface(reader);
        MediaExtractor extractor = new MediaExtractor();
        long firstImage = 0;
        try (FileInputStream input = new FileInputStream(args[1])) {
            extractor.setDataSource(input.getFD());
            extractor.selectTrack(0);
            MediaFormat format = extractor.getTrackFormat(0);
            MediaCodec codec = MediaCodec.createDecoderByType(format.getString(MediaFormat.KEY_MIME));
            try {
                codec.configure(format, surface, null, 0);
                codec.start();
                MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
                boolean sentEos = false;
                int frames = 0;
                long deadline = System.nanoTime() + 20_000_000_000L;
                while (frames < 2 && System.nanoTime() < deadline) {
                    if (!sentEos) {
                        int i = codec.dequeueInputBuffer(10_000);
                        if (i >= 0) {
                            int size = extractor.readSampleData(codec.getInputBuffer(i), 0);
                            if (size >= 0) {
                                codec.queueInputBuffer(i, 0, size, extractor.getSampleTime(), 0);
                                extractor.advance();
                            } else {
                                codec.queueInputBuffer(i, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                                sentEos = true;
                            }
                        }
                    }
                    int i = codec.dequeueOutputBuffer(info, 10_000);
                    if (i < 0) continue;
                    if (info.size == 0) {
                        codec.releaseOutputBuffer(i, false);
                        if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) break;
                        continue;
                    }
                    long timestamp = frames == 0 ? FIRST_TIMESTAMP_NS : SECOND_TIMESTAMP_NS;
                    codec.releaseOutputBuffer(i, timestamp);
                    long image = checkImage(reader, timestamp, frames);
                    if (frames == 0) firstImage = image;
                    else deleteImage(image);
                    ++frames;
                }
                if (frames != 2) throw new AssertionError("Decoded " + frames + " YUV images");
                checkRetainedImage(firstImage);
                System.out.println("NDK YUV image reader: two decoded frames, timestamps, planes and retained pixels passed");
            } finally {
                codec.release();
            }
        } finally {
            if (firstImage != 0) deleteImage(firstImage);
            extractor.release();
            surface.release();
            deleteReader(reader);
        }
    }
}
