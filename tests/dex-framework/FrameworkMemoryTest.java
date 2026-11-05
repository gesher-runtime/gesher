package gesher;

import java.io.FileDescriptor;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.Arrays;

/** Real boot-framework classes and JNI, without compile-time Android stubs. */
public final class FrameworkMemoryTest {
  interface Action { void run() throws Exception; }

  static void check(boolean value, String message) {
    if (!value) throw new AssertionError(message);
  }

  static void expect(Class<? extends Throwable> type, Action action) throws Exception {
    try { action.run(); }
    catch (InvocationTargetException e) {
      if (type.isInstance(e.getCause())) return;
      throw e;
    }
    throw new AssertionError("expected " + type.getName());
  }

  public static void main(String[] args) throws Exception {
    System.loadLibrary("android_runtime");
    memoryFile();
    memoryIntArray();
    System.out.println("FrameworkMemoryTest all passed");
  }

  static void memoryFile() throws Exception {
    Class<?> type = Class.forName("android.os.MemoryFile");
    check(type.getClassLoader() == Object.class.getClassLoader(), "real MemoryFile class");
    Object file = type.getConstructor(String.class, int.class).newInstance("jni-memory", 16);
    Method close = type.getMethod("close");
    try {
      check((Integer) type.getMethod("length").invoke(file) == 16, "MemoryFile length");
      FileDescriptor fd = (FileDescriptor) type.getMethod("getFileDescriptor").invoke(file);
      Method size = type.getMethod("getSize", FileDescriptor.class);
      check((Integer) size.invoke(null, fd) == 16, "native MemoryFile size");
      check(!(Boolean) type.getMethod("isPurgingAllowed").invoke(file), "initial pin policy");
      Method allow = type.getMethod("allowPurging", boolean.class);
      check(!(Boolean) allow.invoke(file, true), "unpin returns previous policy");
      check((Boolean) allow.invoke(file, false), "repin returns previous policy");
      Method write = type.getMethod("writeBytes", byte[].class, int.class, int.class, int.class);
      Method read = type.getMethod("readBytes", byte[].class, int.class, int.class, int.class);
      byte[] payload = {3, 5, 7, 9};
      write.invoke(file, payload, 1, 4, 3);
      byte[] result = new byte[5];
      check((Integer) read.invoke(file, result, 4, 1, 3) == 3, "MemoryFile read length");
      check(Arrays.equals(result, new byte[] {0, 5, 7, 9, 0}), "offset storage round trip");
      expect(IndexOutOfBoundsException.class, () -> write.invoke(file, payload, 0, 15, 4));
      expect(IndexOutOfBoundsException.class, () -> read.invoke(file, result, 0, -1, 1));
      OutputStream output = (OutputStream) type.getMethod("getOutputStream").invoke(file);
      output.write(new byte[] {11, 12, 13});
      InputStream input = (InputStream) type.getMethod("getInputStream").invoke(file);
      check(input.read() == 11 && input.skip(1) == 1 && input.read() == 13, "stream access");
      close.invoke(file);
      expect(IOException.class, () -> read.invoke(file, result, 0, 0, 1));
      expect(IOException.class, () -> size.invoke(null, new FileDescriptor()));
    } finally {
      close.invoke(file); // idempotent
    }
  }

  static void memoryIntArray() throws Exception {
    Class<?> type = Class.forName("android.util.MemoryIntArray");
    check(type.getClassLoader() == Object.class.getClassLoader(), "real MemoryIntArray class");
    Object owner = type.getConstructor(int.class).newInstance(4);
    Method get = type.getMethod("get", int.class);
    Method set = type.getMethod("set", int.class, int.class);
    Method close = type.getMethod("close");
    Class<?> parcelClass = Class.forName("android.os.Parcel");
    Object parcel = parcelClass.getMethod("obtain").invoke(null);
    Object reader = null;
    try {
      check((Boolean) type.getMethod("isWritable").invoke(owner), "owner is writable");
      check((Integer) type.getMethod("size").invoke(owner) == 4, "native array size");
      for (int i = 0; i < 4; i++) check((Integer) get.invoke(owner, i) == 0, "zero initialized");
      set.invoke(owner, 0, Integer.MIN_VALUE);
      set.invoke(owner, 3, Integer.MAX_VALUE);
      expect(IndexOutOfBoundsException.class, () -> get.invoke(owner, -1));
      expect(IndexOutOfBoundsException.class, () -> set.invoke(owner, 4, 99));
      type.getMethod("writeToParcel", parcelClass, int.class).invoke(owner, parcel, 0);
      parcelClass.getMethod("setDataPosition", int.class).invoke(parcel, 0);
      Object creator = type.getField("CREATOR").get(null);
      reader = Class.forName("android.os.Parcelable$Creator")
          .getMethod("createFromParcel", parcelClass).invoke(creator, parcel);
      check(!(Boolean) type.getMethod("isWritable").invoke(reader), "parcel reader is read-only");
      check((Integer) get.invoke(reader, 0) == Integer.MIN_VALUE, "parcel fd shares storage");
      Object readOnly = reader;
      expect(UnsupportedOperationException.class, () -> set.invoke(readOnly, 0, 17));
      set.invoke(owner, 2, 0x12345678);
      check((Integer) get.invoke(reader, 2) == 0x12345678, "live atomic update visible");
      close.invoke(owner);
      check((Boolean) type.getMethod("isClosed").invoke(owner), "owner closed");
      expect(IllegalStateException.class, () -> get.invoke(owner, 0));
      check((Integer) get.invoke(reader, 3) == Integer.MAX_VALUE, "reader owns independent fd");
      close.invoke(reader);
      expect(IllegalStateException.class, () -> get.invoke(readOnly, 0));
      expect(IOException.class, () -> type.getConstructor(int.class).newInstance(0));
      expect(IllegalArgumentException.class, () -> type.getConstructor(int.class).newInstance(1025));
    } finally {
      if (reader != null) close.invoke(reader);
      close.invoke(owner);
      parcelClass.getMethod("recycle").invoke(parcel);
    }
  }
}
