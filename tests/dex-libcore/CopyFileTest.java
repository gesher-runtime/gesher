package gesher;

import java.io.FileDescriptor;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.file.FileAlreadyExistsException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.Arrays;

public final class CopyFileTest {
  private static Method transfer;
  private static Method descriptor;

  private static void check(boolean okay, String name) {
    if (!okay) throw new AssertionError(name);
    System.out.println("PASS  libcore copy: " + name);
  }

  private static byte[] bytes(int length) {
    byte[] out = new byte[length];
    for (int i = 0; i < length; ++i) out[i] = (byte) (i * 37 + (i >>> 8));
    return out;
  }

  private static int fd(RandomAccessFile file) throws Exception {
    return (Integer) descriptor.invoke(file.getFD());
  }

  private static void copy(int destination, int source, long cancel) throws Exception {
    transfer.invoke(null, destination, source, cancel);
  }

  private static void error(int destination, int source, long cancel, int expected) throws Exception {
    try {
      copy(destination, source, cancel);
      throw new AssertionError("missing UnixException " + expected);
    } catch (InvocationTargetException exception) {
      Throwable cause = exception.getCause();
      if (!cause.getClass().getName().equals("sun.nio.fs.UnixException")) throw exception;
      Method errno = cause.getClass().getDeclaredMethod("errno");
      errno.setAccessible(true);
      if ((Integer) errno.invoke(cause) != expected) throw new AssertionError(cause);
    }
  }

  public static void main(String[] args) throws Exception {
    Path root = Path.of(args[0]);
    Files.createDirectories(root);
    for (int size : new int[] {0, 1, 8191, 8192, 8193, 2 * 1024 * 1024 + 71}) {
      Path source = root.resolve("source-" + size + "-\u05d2.bin");
      Path destination = root.resolve("copy-" + size + "-\u05e9.bin");
      byte[] expected = bytes(size);
      Files.write(source, expected);
      check(Files.copy(source, destination).equals(destination) &&
          Arrays.equals(expected, Files.readAllBytes(destination)), "Files.copy exact bytes " + size);
    }
    Path source = root.resolve("replace-source.bin"), destination = root.resolve("replace-target.bin");
    byte[] original = bytes(13), replacement = bytes(40001);
    Files.write(source, replacement);
    Files.write(destination, original);
    boolean exists = false;
    try { Files.copy(source, destination); }
    catch (FileAlreadyExistsException expected) { exists = true; }
    check(exists && Arrays.equals(original, Files.readAllBytes(destination)),
        "existing destination is preserved without replacement");
    Files.copy(source, destination, StandardCopyOption.REPLACE_EXISTING);
    check(Arrays.equals(replacement, Files.readAllBytes(destination)), "explicit replacement copies every byte");
    Path missingTarget = root.resolve("missing-target.bin");
    boolean missing = false;
    try { Files.copy(root.resolve("missing-source.bin"), missingTarget); }
    catch (IOException expected) { missing = true; }
    check(missing && !Files.exists(missingTarget), "missing source leaves no partial destination");

    transfer = Class.forName("sun.nio.fs.UnixCopyFile").getDeclaredMethod(
        "transfer", int.class, int.class, long.class);
    transfer.setAccessible(true);
    descriptor = FileDescriptor.class.getMethod("getInt$");
    Class<?> unsafeClass = Class.forName("jdk.internal.misc.Unsafe");
    Field singleton = unsafeClass.getDeclaredField("theUnsafe");
    singleton.setAccessible(true);
    Object unsafe = singleton.get(null);
    Method allocate = unsafeClass.getMethod("allocateMemory", long.class);
    Method release = unsafeClass.getMethod("freeMemory", long.class);
    Method put = unsafeClass.getMethod("putInt", long.class, int.class);
    long cancel = (Long) allocate.invoke(unsafe, 4L);
    if (cancel == 0) throw new AssertionError("real cancellation word allocation");
    try (RandomAccessFile input = new RandomAccessFile(source.toFile(), "r");
         RandomAccessFile output = new RandomAccessFile(destination.toFile(), "rw")) {
      put.invoke(unsafe, cancel, 0);
      output.setLength(9);
      output.seek(0);
      output.write(original, 0, 9);
      output.seek(9);
      input.seek(17);
      copy(fd(output), fd(input), cancel);
      byte[] result = new byte[9 + replacement.length - 17];
      System.arraycopy(original, 0, result, 0, 9);
      System.arraycopy(replacement, 17, result, 9, replacement.length - 17);
      check(Arrays.equals(result, Files.readAllBytes(destination)) &&
          input.getFilePointer() == replacement.length && output.getFilePointer() == result.length,
          "native transfer honors both offsets and retains caller descriptor ownership");

      put.invoke(unsafe, cancel, 1);
      input.seek(0);
      output.setLength(0);
      error(fd(output), fd(input), cancel, 125);
      check(input.getFilePointer() == 8192 && output.length() == 0,
          "real cancellation is checked after read and before any write");
      error(fd(output), -1, cancel, 9);
      check(output.length() == 0, "source EBADF precedes cancellation");
      input.seek(input.length());
      copy(-1, fd(input), cancel);
      check(output.length() == 0, "EOF precedes cancellation and destination validation");

      put.invoke(unsafe, cancel, 0);
      input.seek(0);
      error(-1, fd(input), cancel, 9);
      check(input.getFilePointer() == 8192, "destination EBADF follows the actual source read");
      input.seek(0);
      output.seek(0);
      copy(fd(output), fd(input), cancel);
      check(Arrays.equals(replacement, Files.readAllBytes(destination)),
          "clearing the same cancellation word permits a complete real transfer");
    } finally { release.invoke(unsafe, cancel); }
    System.out.println("CopyFileTest all passed");
  }
}
