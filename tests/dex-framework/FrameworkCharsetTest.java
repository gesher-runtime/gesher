package gesher;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInput;
import java.io.DataOutput;
import java.io.Flushable;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.IOException;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.Arrays;

/** Exact Android JNI bytes, capacity contracts and decoder buffer ownership. */
public final class FrameworkCharsetTest {
  interface Action { void run() throws Exception; }
  static Object unsafe;
  static Method putByte, getByte, encode, decode;

  static void check(boolean ok, String message) {
    if (!ok) throw new AssertionError(message);
  }

  static void expect(Class<? extends Throwable> type, Action action) throws Exception {
    try { action.run(); }
    catch (InvocationTargetException e) {
      if (type.isInstance(e.getCause())) return;
      throw e;
    }
    throw new AssertionError("expected " + type.getName());
  }

  static void fill(long address, int size) throws Exception {
    for (int i = 0; i < size; i++) putByte.invoke(unsafe, address + i, (byte) 0x55);
  }

  static byte[] bytes(long address, int length) throws Exception {
    byte[] result = new byte[length];
    for (int i = 0; i < length; i++) result[i] = (Byte) getByte.invoke(unsafe, address + i);
    return result;
  }

  static void vector(long memory, String value, int... expected) throws Exception {
    fill(memory, 128);
    int count = (Integer) encode.invoke(null, value, memory, 3, 128);
    check(count == expected.length, "encoded length " + value.length());
    byte[] wanted = new byte[expected.length];
    for (int i = 0; i < wanted.length; i++) wanted[i] = (byte) expected[i];
    check(Arrays.equals(bytes(memory + 3, count), wanted), "exact encoded bytes");
    check((Byte) getByte.invoke(unsafe, memory + 2) == 0x55 &&
        (Byte) getByte.invoke(unsafe, memory + 3 + count) == 0 &&
        (Byte) getByte.invoke(unsafe, memory + 4 + count) == 0x55, "encoder boundaries");
    // Decoding borrows one extra byte for a terminator, then restores it.
    putByte.invoke(unsafe, memory + 3 + count, (byte) 0x66);
    check(value.equals(decode.invoke(null, memory, 3, count)), "round trip");
    check((Byte) getByte.invoke(unsafe, memory + 3 + count) == 0x66, "decoder restored sentinel");
    fill(memory, 128);
    check((Integer) encode.invoke(null, value, memory, 3, 3 + count) == -count,
        "capacity without terminator rejected");
    check((Byte) getByte.invoke(unsafe, memory + 3) == 0x55, "short buffer unchanged");
    check((Integer) encode.invoke(null, value, memory, 3, 4 + count) == count,
        "exact capacity with terminator accepted");
  }

  static void fastData() throws Exception {
    // Real framework callers exercise the negative-size/retry path as their
    // eight-byte buffers overflow, including temporary nonmoving allocations.
    ByteArrayOutputStream wire = new ByteArrayOutputStream();
    Object output = Class.forName("com.android.internal.util.ArtFastDataOutput")
        .getConstructor(OutputStream.class, int.class).newInstance(wire, 8);
    DataOutput writer = (DataOutput) output;
    char[] large = new char[1024];
    Arrays.fill(large, '\u05e9');
    String[] values = {"", "ascii", "x\u0000y", "\ud83d\ude80", "\ud800", new String(large)};
    writer.writeInt(0x12345678);
    for (String value : values) writer.writeUTF(value);
    ((Flushable) output).flush();
    DataInput reader = (DataInput) Class.forName("com.android.internal.util.ArtFastDataInput")
        .getConstructor(InputStream.class, int.class)
        .newInstance(new ByteArrayInputStream(wire.toByteArray()), 8);
    check(reader.readInt() == 0x12345678, "FastData primitive framing");
    for (String value : values) check(value.equals(reader.readUTF()), "FastData UTF round trip");
    char[] oversized = new char[65536];
    Arrays.fill(oversized, 'a');
    try {
      writer.writeUTF(new String(oversized));
      throw new AssertionError("oversize UTF accepted");
    } catch (IOException expected) { }
  }

  public static void main(String[] args) throws Exception {
    System.loadLibrary("android_runtime");
    Class<?> type = Class.forName("android.util.CharsetUtils");
    check(type.getClassLoader() == Object.class.getClassLoader(), "real CharsetUtils class");
    encode = type.getMethod("toModifiedUtf8Bytes", String.class, long.class, int.class, int.class);
    decode = type.getMethod("fromModifiedUtf8Bytes", long.class, int.class, int.class);
    Class<?> unsafeClass = Class.forName("jdk.internal.misc.Unsafe");
    Field singleton = unsafeClass.getDeclaredField("theUnsafe");
    singleton.setAccessible(true);
    unsafe = singleton.get(null);
    putByte = unsafeClass.getMethod("putByte", long.class, byte.class);
    getByte = unsafeClass.getMethod("getByte", long.class);
    long memory = (Long) unsafeClass.getMethod("allocateMemory", long.class).invoke(unsafe, 128L);
    check(memory != 0, "native buffer allocated");
    try {
      vector(memory, "");
      vector(memory, "ASCII", 65, 83, 67, 73, 73);
      vector(memory, "a\u0000b", 97, 0xc0, 0x80, 98);
      vector(memory, "\u05e9\u20ac", 0xd7, 0xa9, 0xe2, 0x82, 0xac);
      vector(memory, "\ud83d\ude80", 0xf0, 0x9f, 0x9a, 0x80);
      vector(memory, "\ud800", 0xed, 0xa0, 0x80);
      vector(memory, "\udc00", 0xed, 0xb0, 0x80);
      check((Integer) encode.invoke(null, "abc", memory, -1, 128) == -3, "negative offset sizing");
      check((Integer) encode.invoke(null, "abc", memory, Integer.MAX_VALUE, Integer.MAX_VALUE) == -3,
          "overflowing offset sizing");
      check((Integer) encode.invoke(null, "abc", memory, 0, -1) == -3, "negative capacity sizing");
      check((Integer) encode.invoke(null, "abc", 0L, 0, 0) == -3, "null destination size query");
      expect(NullPointerException.class, () -> encode.invoke(null, null, memory, 0, 128));
      expect(NullPointerException.class, () -> encode.invoke(null, "abc", 0L, 0, 128));
      expect(NullPointerException.class, () -> decode.invoke(null, 0L, 0, 0));
      expect(IndexOutOfBoundsException.class, () -> decode.invoke(null, memory, -1, 1));
      expect(IndexOutOfBoundsException.class, () -> decode.invoke(null, memory, 0, -1));
      expect(IndexOutOfBoundsException.class,
          () -> decode.invoke(null, memory, Integer.MAX_VALUE, 1));
      fastData();
      System.out.println("FrameworkCharsetTest all passed");
    } finally {
      unsafeClass.getMethod("freeMemory", long.class).invoke(unsafe, memory);
    }
  }
}
