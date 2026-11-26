package android.content;

import android.util.ArrayMap;
import java.util.Map;
import java.util.Set;

/** android.content.ContentValues with android-16.0.0_r4's conversion rules. */
public final class ContentValues {
    private final ArrayMap<String, Object> mMap;
    public ContentValues() { mMap = new ArrayMap<>(); }
    public ContentValues(ContentValues from) { mMap = new ArrayMap<>(from.mMap); }
    public ArrayMap<String, Object> getValues() { return mMap; }
    public void put(String key, String value) { mMap.put(key, value); }
    public void put(String key, Integer value) { mMap.put(key, value); }
    public void put(String key, Long value) { mMap.put(key, value); }
    public void put(String key, Boolean value) { mMap.put(key, value); }
    public void putNull(String key) { mMap.put(key, null); }
    public Object get(String key) { return mMap.get(key); }
    public boolean containsKey(String key) { return mMap.containsKey(key); }
    public void remove(String key) { mMap.remove(key); }
    public int size() { return mMap.size(); }
    public boolean isEmpty() { return mMap.isEmpty(); }
    public Set<Map.Entry<String, Object>> valueSet() { return mMap.entrySet(); }
    public Set<String> keySet() { return mMap.keySet(); }
    public String getAsString(String key) {
        Object value = mMap.get(key);
        return value != null ? value.toString() : null;
    }
    public Long getAsLong(String key) {
        Object value = mMap.get(key);
        if (value == null) return null;
        if (value instanceof Number) return ((Number) value).longValue();
        if (value instanceof CharSequence) {
            try { return Long.valueOf(value.toString()); } catch (NumberFormatException e) { return null; }
        }
        return null;
    }
    public Integer getAsInteger(String key) {
        Object value = mMap.get(key);
        if (value == null) return null;
        if (value instanceof Number) return ((Number) value).intValue();
        if (value instanceof CharSequence) {
            try { return Integer.valueOf(value.toString()); } catch (NumberFormatException e) { return null; }
        }
        return null;
    }
    public Boolean getAsBoolean(String key) {
        Object value = mMap.get(key);
        if (value == null || value instanceof Boolean) return (Boolean) value;
        if (value instanceof CharSequence) return Boolean.valueOf(value.toString()) || "1".equals(value);
        if (value instanceof Number) return ((Number) value).intValue() != 0;
        return null;
    }
    @Override public String toString() { return mMap.toString(); }
}
