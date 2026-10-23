package android.util;

import java.util.Arrays;

/** Stand-in for the framework class: sorted int keys, the methods MessageQueue uses. */
public class SparseArray<E> {
  private int[] mKeys = new int[4];
  private Object[] mValues = new Object[4];
  private int mSize;

  public int size() {
    return mSize;
  }

  public int indexOfKey(int key) {
    return Arrays.binarySearch(mKeys, 0, mSize, key);
  }

  @SuppressWarnings("unchecked")
  public E get(int key) {
    int i = indexOfKey(key);
    return i < 0 ? null : (E) mValues[i];
  }

  @SuppressWarnings("unchecked")
  public E valueAt(int index) {
    return (E) mValues[index];
  }

  public int keyAt(int index) {
    return mKeys[index];
  }

  public void put(int key, E value) {
    int i = indexOfKey(key);
    if (i >= 0) {
      mValues[i] = value;
      return;
    }
    i = ~i;
    if (mSize == mKeys.length) {
      mKeys = Arrays.copyOf(mKeys, mSize * 2);
      mValues = Arrays.copyOf(mValues, mSize * 2);
    }
    System.arraycopy(mKeys, i, mKeys, i + 1, mSize - i);
    System.arraycopy(mValues, i, mValues, i + 1, mSize - i);
    mKeys[i] = key;
    mValues[i] = value;
    mSize++;
  }

  public void removeAt(int index) {
    System.arraycopy(mKeys, index + 1, mKeys, index, mSize - index - 1);
    System.arraycopy(mValues, index + 1, mValues, index, mSize - index - 1);
    mSize--;
    mValues[mSize] = null;
  }
}
