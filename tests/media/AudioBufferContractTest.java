/* Real host JNI tests. The native fixture observes/replaces only the audio engine. */
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public final class AudioBufferContractTest {
    private native int native_write_byte(byte[] data, int offset, int size, int format, boolean block);
    private native int native_write_short(short[] data, int offset, int size, int format, boolean block);
    private native int native_write_float(float[] data, int offset, int size, int format, boolean block);
    private native int native_write_native_bytes(ByteBuffer data, int offset, int size, int format, boolean block);
    private native int native_read_in_byte_array(byte[] data, int offset, int size, boolean block);
    private native int native_read_in_short_array(short[] data, int offset, int size, boolean block);
    private native int native_read_in_float_array(float[] data, int offset, int size, boolean block);
    private native int native_read_in_direct_buffer(Object data, int size, boolean block);
    private static native void reset();
    private static native long observed(int field);
    private static native void inspectPayload();
    private static native byte[] payload();
    private static int checks;

    private static void equal(long expected, long actual, String message) {
        checks++;
        if (actual != expected) throw new AssertionError(message + ": expected " + expected + ", got " + actual);
    }

    private void run(String which) {
        reset();
        switch (which) {
            case "valid": {
                inspectPayload();
                byte[] bytes = {1, 2, 3, 4};
                equal(2, native_write_byte(bytes, 1, 2, 3, true), "byte slice count");
                equal(2, observed(1), "byte backend size");
                equal(1, observed(2), "blocking forwarded");
                equal(3, bytes[2], "write source preserved");
                equal(2, payload()[0], "byte payload starts at offset");
                equal(3, payload()[1], "byte payload ends at slice boundary");
                equal(2, native_write_short(new short[] {1, 2, 3, 4}, 1, 2, 2, false), "short count");
                equal(4, observed(1), "short byte size");
                equal(2, ByteBuffer.wrap(payload()).order(ByteOrder.nativeOrder()).getShort(), "short payload offset");
                equal(0, observed(2), "nonblocking forwarded");
                equal(2, native_write_float(new float[] {1, 2, 3, 4}, 1, 2, 4, true), "float count");
                equal(8, observed(1), "float byte size");
                equal(Float.floatToRawIntBits(2), ByteBuffer.wrap(payload()).order(ByteOrder.nativeOrder()).getInt(), "float payload offset");
                equal(0, native_write_byte(bytes, bytes.length, 0, 3, false), "empty end slice");
                equal(2, native_read_in_byte_array(bytes, 1, 2, false), "read byte count");
                equal(1, bytes[0], "read prefix retained");
                equal(0x3c, bytes[1], "read first byte");
                equal(0x3c, bytes[2], "read last byte");
                equal(4, bytes[3], "read suffix retained");
                short[] shorts = {1, 2, 3, 4};
                equal(2, native_read_in_short_array(shorts, 1, 2, true), "read short count");
                equal(4, observed(1), "read short byte size");
                equal(1, shorts[0], "short prefix retained");
                equal(0x3c3c, shorts[1], "short copy back");
                equal(4, shorts[3], "short suffix retained");
                float[] floats = {1, 2, 3, 4};
                equal(2, native_read_in_float_array(floats, 1, 2, false), "read float count");
                equal(8, observed(1), "read float byte size");
                equal(0x3c3c3c3c, Float.floatToRawIntBits(floats[1]), "float copy back");
                equal(Float.floatToRawIntBits(4), Float.floatToRawIntBits(floats[3]), "float suffix retained");
                ByteBuffer direct = ByteBuffer.allocateDirect(8);
                direct.put(2, (byte) 41).put(4, (byte) 43);
                equal(3, native_write_native_bytes(direct, 2, 3, 3, false), "direct write");
                equal(41, payload()[0], "direct payload starts at byte offset");
                equal(43, payload()[2], "direct payload ends at byte offset plus count");
                break;
            }
            case "record-capacity": {
                ByteBuffer data = ByteBuffer.allocateDirect(16);
                data.position(7).limit(9);
                equal(16, native_read_in_direct_buffer(data, Integer.MAX_VALUE, true), "oversized request clamps");
                equal(16, observed(1), "capacity bounds actual transfer");
                equal(7, data.position(), "position preserved");
                equal(9, data.limit(), "limit preserved");
                data.clear();
                for (int i = 0; i < 16; i++) equal(0x3c, data.get(i), "data starts at buffer origin");
                equal(0, native_read_in_direct_buffer(data, 0, false), "zero request");
                // Match the pinned Android JNI behavior, whose non-direct error
                // differs from the Java documentation's historical wording.
                equal(-2, native_read_in_direct_buffer(ByteBuffer.allocate(16), 4, false), "heap buffer rejected");
                break;
            }
            case "record-bounds": {
                equal(-2, native_read_in_byte_array(new byte[4], 2, Integer.MAX_VALUE, false), "byte overflow");
                equal(-2, native_read_in_short_array(new short[4], 2, Integer.MAX_VALUE, false), "short overflow");
                equal(-2, native_read_in_float_array(new float[4], 2, Integer.MAX_VALUE, false), "float overflow");
                equal(-2, native_read_in_byte_array(new byte[4], 3, 2, false), "read beyond end");
                equal(-2, native_read_in_direct_buffer(ByteBuffer.allocateDirect(8), -1, false), "negative direct read");
                equal(0, observed(0), "invalid reads never reach capture");
                break;
            }
            case "track-bounds": {
                equal(-2, native_write_byte(new byte[4], 3, 2, 3, false), "byte beyond end");
                equal(-2, native_write_short(new short[4], 3, 2, 2, false), "short beyond end");
                equal(-2, native_write_float(new float[4], 3, 2, 4, false), "float beyond end");
                equal(-2, native_write_byte(new byte[4], 2, Integer.MAX_VALUE, 3, false), "huge write rejected before allocation");
                equal(0, observed(0), "invalid writes never reach playback");
                break;
            }
            case "direct-bounds": {
                ByteBuffer data = ByteBuffer.allocateDirect(8);
                equal(-2, native_write_native_bytes(data, 7, 2, 3, false), "direct end overflow");
                equal(-2, native_write_native_bytes(data, 2, Integer.MAX_VALUE, 3, false), "direct integer overflow");
                equal(-2, native_write_native_bytes(data, Integer.MAX_VALUE, 1, 3, false), "direct invalid start");
                equal(-2, native_write_native_bytes(data, 0, -1, 3, false), "negative direct write");
                equal(0, observed(0), "invalid direct writes never reach playback");
                break;
            }
            case "null-direct": {
                equal(-2, native_write_native_bytes(null, 0, 0, 3, false), "null direct write");
                equal(-2, native_read_in_direct_buffer(null, 0, false), "null direct read");
                equal(0, observed(0), "null buffers never reach engines");
                break;
            }
            default: throw new AssertionError(which);
        }
    }

    public static void main(String[] args) {
        System.load(args[0]);
        new AudioBufferContractTest().run(args[1]);
        System.out.println("PASS " + args[1] + " (" + checks + " assertions)");
    }
}
