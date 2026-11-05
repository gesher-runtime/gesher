package gesher;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/** RFC 7914 known answers, invalid costs, nulls and input ownership over JNI. */
public final class FrameworkScryptTest {
  static final Class<?>[] SIGNATURE = {byte[].class, byte[].class, int.class, int.class,
      int.class, int.class};

  static void check(boolean ok, String message) {
    if (!ok) throw new AssertionError(message);
  }

  static String hex(byte[] bytes) {
    StringBuilder result = new StringBuilder();
    for (byte b : bytes) result.append(String.format("%02x", b & 255));
    return result.toString();
  }

  public static void main(String[] args) throws Exception {
    System.loadLibrary("android_runtime");
    Class<?> type = Class.forName("android.security.Scrypt");
    check(type.getClassLoader() == Object.class.getClassLoader(), "real Scrypt class");
    Object scrypt = type.getConstructor().newInstance();
    Method derive = type.getMethod("scrypt", SIGNATURE);
    byte[] empty = new byte[0];
    byte[] key = (byte[]) derive.invoke(scrypt, empty, empty, 16, 1, 1, 64);
    check(hex(key).equals("77d6576238657b203b19ca42c18a0497f16b4844e3074ae8dfdffa3fede21442"
        + "fcd0069ded0948f8326a753a0fc81f17e8d3e0fb2e0d3628cf35e20c38d18906"),
        "RFC 7914 empty password vector");
    byte[] password = "password".getBytes(StandardCharsets.US_ASCII);
    byte[] salt = "NaCl".getBytes(StandardCharsets.US_ASCII);
    byte[] passwordCopy = password.clone(), saltCopy = salt.clone();
    key = (byte[]) derive.invoke(scrypt, password, salt, 1024, 8, 16, 64);
    check(hex(key).equals("fdbabe1c9d3472007856e7190d01e9fe7c6ad7cbc8237830e77376634b373162"
        + "2eaf30d92e22a3886ff109279d9830dac727afb94a83ee6d8360cbdfa2cc0640"),
        "RFC 7914 password/NaCl vector");
    check(Arrays.equals(password, passwordCopy) && Arrays.equals(salt, saltCopy),
        "input arrays remain unchanged");
    check(derive.invoke(scrypt, null, salt, 16, 1, 1, 32) == null, "null password");
    check(derive.invoke(scrypt, password, null, 16, 1, 1, 32) == null, "null salt");
    for (int[] costs : new int[][] {{0, 1, 1}, {1, 1, 1}, {15, 1, 1}, {-1, 1, 1},
        {16, 0, 1}, {16, -1, 1}, {16, 1, 0}, {16, 1, -1}, {16, 1, Integer.MAX_VALUE}}) {
      check(derive.invoke(scrypt, password, salt, costs[0], costs[1], costs[2], 32) == null,
          "invalid cost rejected: " + Arrays.toString(costs));
    }
    check(((byte[]) derive.invoke(scrypt, password, salt, 16, 1, 1, 0)).length == 0,
        "zero-length result");
    try {
      derive.invoke(scrypt, password, salt, 16, 1, 1, -1);
      throw new AssertionError("negative output length accepted");
    } catch (InvocationTargetException e) {
      check(e.getCause() instanceof NegativeArraySizeException, "negative length exception");
    }
    check(((byte[]) derive.invoke(scrypt, password, salt, 16, 1, 1, 32)).length == 32,
        "valid derivation after failures");
    System.out.println("FrameworkScryptTest all passed");
  }
}
