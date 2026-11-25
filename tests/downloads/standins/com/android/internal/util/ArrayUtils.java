package com.android.internal.util;
public final class ArrayUtils {
    private ArrayUtils() {}
    public static <T> boolean isEmpty(T[] array) { return array == null || array.length == 0; }
}
