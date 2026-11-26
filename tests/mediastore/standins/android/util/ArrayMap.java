package android.util;
/** Insertion-ordered map with ArrayMap's index accessors. */
public final class ArrayMap<K, V> extends java.util.LinkedHashMap<K, V> {
    public ArrayMap() {}
    public ArrayMap(java.util.Map<K, V> other) { super(other); }
    @SuppressWarnings("unchecked")
    public K keyAt(int index) { return (K) keySet().toArray()[index]; }
    @SuppressWarnings("unchecked")
    public V valueAt(int index) { return (V) values().toArray()[index]; }
}
