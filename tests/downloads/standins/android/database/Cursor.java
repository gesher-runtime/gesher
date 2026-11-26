package android.database;
/** The android.database.Cursor methods the provider and its tests use. */
public interface Cursor extends java.io.Closeable {
    int FIELD_TYPE_NULL = 0;
    int getCount();
    boolean moveToFirst();
    boolean moveToNext();
    int getColumnIndex(String column);
    int getColumnIndexOrThrow(String column);
    String[] getColumnNames();
    String getString(int column);
    int getInt(int column);
    long getLong(int column);
    boolean isNull(int column);
    @Override void close();
}
