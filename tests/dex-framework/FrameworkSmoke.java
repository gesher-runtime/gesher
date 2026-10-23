package gesher;

import java.lang.reflect.Method;

/**
 * framework.jar on the boot class path: its classes define, link and run
 * where they need no natives (ArrayMap, Uri, TextUtils), through reflection,
 * so this compiles without the framework.
 */
public final class FrameworkSmoke {
  public static void main(String[] args) throws Exception {
    Class<?> arrayMap = Class.forName("android.util.ArrayMap");
    Object map = arrayMap.getConstructor().newInstance();
    arrayMap.getMethod("put", Object.class, Object.class).invoke(map, "gesher", 42);
    check(Integer.valueOf(42).equals(arrayMap.getMethod("get", Object.class).invoke(map, "gesher")),
        "ArrayMap put/get");

    Class<?> uri = Class.forName("android.net.Uri");
    Object parsed = uri.getMethod("parse", String.class).invoke(null, "https://example.org/a/b?q=1");
    check("example.org".equals(uri.getMethod("getHost").invoke(parsed)), "Uri.getHost");
    check("1".equals(uri.getMethod("getQueryParameter", String.class).invoke(parsed, "q")),
        "Uri.getQueryParameter");

    Method isEmpty = Class.forName("android.text.TextUtils").getMethod("isEmpty", CharSequence.class);
    check((Boolean) isEmpty.invoke(null, "") && !(Boolean) isEmpty.invoke(null, "x"),
        "TextUtils.isEmpty");

    // The framework's own classes come from the boot class loader.
    check(arrayMap.getClassLoader() == Object.class.getClassLoader(), "boot class loader");
    System.out.println("FrameworkSmoke all passed");
  }

  private static void check(boolean ok, String what) {
    if (!ok) throw new AssertionError(what);
  }
}
