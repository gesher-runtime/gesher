package gesher;

import java.io.File;
import java.io.FileDescriptor;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.Arrays;

/** Filesystem local streams and explicit errors for unavailable host features. */
public final class FrameworkLocalSocketTest {
  interface Action { void run() throws Exception; }

  static void check(boolean ok, String message) {
    if (!ok) throw new AssertionError(message);
  }

  static void ioFailure(Action action) throws Exception {
    try { action.run(); }
    catch (IOException e) { return; }
    catch (InvocationTargetException e) {
      if (e.getCause() instanceof IOException) return;
      throw e;
    }
    throw new AssertionError("expected IOException");
  }

  static void readFully(InputStream input, byte[] data, int offset, int length) throws Exception {
    while (length != 0) {
      int count = input.read(data, offset, length);
      check(count > 0, "unexpected EOF");
      offset += count;
      length -= count;
    }
  }

  public static void main(String[] args) throws Exception {
    System.loadLibrary("android_runtime");
    Class<?> type = Class.forName("android.net.LocalSocket");
    Class<?> address = Class.forName("android.net.LocalSocketAddress");
    Class<?> namespace = Class.forName("android.net.LocalSocketAddress$Namespace");
    Class<?> serverType = Class.forName("android.net.LocalServerSocket");
    check(type.getClassLoader() == Object.class.getClassLoader(), "real LocalSocket class");
    Object filesystem = namespace.getField("FILESYSTEM").get(null);
    // /tmp is not a guaranteed host directory on a fresh Windows runner.
    // The launcher supplies an owned short directory; direct runs use their
    // disposable ANDROID_DATA directory rather than a machine-global /tmp.
    String directoryPath = args.length == 1 ? args[0] : System.getenv("ANDROID_DATA");
    check(directoryPath != null, "provide an endpoint directory or ANDROID_DATA");
    File directory = new File(directoryPath);
    check(directory.isDirectory() || directory.mkdirs(), "create endpoint directory");
    File endpoint = File.createTempFile("gsl-", ".sock", directory);
    check(endpoint.delete(), "remove endpoint placeholder");
    Object localAddress = address.getConstructor(String.class, namespace)
        .newInstance(endpoint.getAbsolutePath(), filesystem);
    Object listener = type.getConstructor().newInstance();
    Object client = type.getConstructor().newInstance();
    Object server = null, accepted = null;
    Method close = type.getMethod("close");
    try {
      type.getMethod("bind", address).invoke(listener, localAddress);
      FileDescriptor listenerFd = (FileDescriptor) type.getMethod("getFileDescriptor").invoke(listener);
      server = serverType.getConstructor(FileDescriptor.class).newInstance(listenerFd);
      type.getMethod("connect", address).invoke(client, localAddress);
      accepted = serverType.getMethod("accept").invoke(server);
      InputStream incoming = (InputStream) type.getMethod("getInputStream").invoke(accepted);
      OutputStream outgoing = (OutputStream) type.getMethod("getOutputStream").invoke(client);
      outgoing.write(255);
      outgoing.write(new byte[] {10, 11, 12, 13, 14}, 1, 3);
      check(incoming.read() == 255, "unsigned single-byte native read/write");
      byte[] payload = new byte[5];
      readFully(incoming, payload, 1, 3);
      check(Arrays.equals(payload, new byte[] {0, 11, 12, 13, 0}), "offset array transfer");
      check(incoming.read(payload, 0, 0) == 0, "zero-length read");
      try {
        outgoing.write(payload, Integer.MAX_VALUE, 2);
        throw new AssertionError("overflowing offset accepted");
      } catch (IndexOutOfBoundsException expected) { }
      FileDescriptor clientFd = (FileDescriptor) type.getMethod("getFileDescriptor").invoke(client);
      type.getMethod("setFileDescriptorsForSend", FileDescriptor[].class)
          .invoke(client, (Object) new FileDescriptor[] {clientFd});
      ioFailure(() -> outgoing.write(99));
      type.getMethod("setFileDescriptorsForSend", FileDescriptor[].class)
          .invoke(client, (Object) null);
      outgoing.write(42);
      check(incoming.read() == 42, "rejected ancillary send wrote no payload");
      Object peer = accepted;
      ioFailure(() -> type.getMethod("getPeerCredentials").invoke(peer));
      check(type.getMethod("getAncillaryFileDescriptors").invoke(accepted) == null,
          "no fabricated inbound descriptors");
      type.getMethod("shutdownOutput").invoke(client);
      check(incoming.read() == -1 && incoming.read(payload) == -1, "half-close EOF");
      OutputStream reply = (OutputStream) type.getMethod("getOutputStream").invoke(accepted);
      InputStream response = (InputStream) type.getMethod("getInputStream").invoke(client);
      reply.write(71);
      check(response.read() == 71, "reverse direction survives half-close");
      close.invoke(accepted);
      check(response.read() == -1, "peer close EOF");
      ioFailure(() -> reply.write(1));
      // A wrapper created with someone else's descriptor does not own it.
      serverType.getMethod("close").invoke(server);
      check(listenerFd.valid(), "LocalServerSocket wrapper preserves supplied fd");
      close.invoke(listener);
      check(!listenerFd.valid(), "owning LocalSocket closes its fd");
      Object absent = address.getConstructor(String.class, namespace)
          .newInstance(endpoint.getAbsolutePath() + ".missing", filesystem);
      Object refused = type.getConstructor().newInstance();
      try { ioFailure(() -> type.getMethod("connect", address).invoke(refused, absent)); }
      finally { close.invoke(refused); }
      Object abstractAddress = address.getConstructor(String.class).newInstance("gs-unsupported");
      Object unsupported = type.getConstructor().newInstance();
      try { ioFailure(() -> type.getMethod("connect", address).invoke(unsupported, abstractAddress)); }
      finally { close.invoke(unsupported); }
      System.out.println("FrameworkLocalSocketTest all passed");
    } finally {
      if (accepted != null) close.invoke(accepted);
      if (server != null) serverType.getMethod("close").invoke(server);
      close.invoke(client);
      close.invoke(listener);
      endpoint.delete();
    }
  }
}
