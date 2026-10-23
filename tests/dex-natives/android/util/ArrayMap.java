package android.util;

import java.util.HashMap;

/** Stand-in for the framework class: a map (AssetManager's natives create and fill one). */
public final class ArrayMap<K, V> extends HashMap<K, V> {
  public ArrayMap() {}
}
