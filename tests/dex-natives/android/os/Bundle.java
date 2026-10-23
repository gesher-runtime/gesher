package android.os;

import java.util.HashMap;

/** Stand-in for the framework class: a map, as much as Message needs. */
public final class Bundle implements Cloneable {
  private final HashMap<String, Object> mMap;

  public Bundle() {
    mMap = new HashMap<>();
  }

  public Bundle(Bundle b) {
    mMap = new HashMap<>(b.mMap);
  }

  public void setClassLoader(ClassLoader loader) {}

  public void putString(String key, String value) {
    mMap.put(key, value);
  }

  public String getString(String key) {
    Object v = mMap.get(key);
    return v instanceof String ? (String) v : null;
  }

  @Override
  public Object clone() {
    return new Bundle(this);
  }
}
