package android.database.sqlite;

import android.content.ContentValues;
import android.database.Cursor;
import android.os.CancellationSignal;

import java.io.File;
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * The android.database.sqlite.SQLiteDatabase methods the downloads provider calls,
 * executed by a real SQLite engine (sqlite-jdbc). Statement text and bindings are
 * the caller's, unchanged; only the native binding layer differs.
 */
public final class SQLiteDatabase {
    public interface CursorFactory {}

    private final Connection mConnection;
    private int mTransactionDepth;
    private boolean mTransactionFailed;
    private final List<Boolean> mSuccessful = new ArrayList<>();

    private SQLiteDatabase(String url) {
        try {
            mConnection = DriverManager.getConnection(url);
        } catch (SQLException e) {
            throw new SQLiteException("open " + url, e);
        }
    }

    public static SQLiteDatabase openOrCreateDatabase(File file, CursorFactory factory) {
        return new SQLiteDatabase("jdbc:sqlite:" + file.getAbsolutePath());
    }

    public static SQLiteDatabase create(CursorFactory factory) {
        return new SQLiteDatabase("jdbc:sqlite::memory:");
    }

    public synchronized void close() {
        try { mConnection.close(); } catch (SQLException e) { throw new SQLiteException("close", e); }
    }

    /** SQLiteDatabase.findEditTable. */
    public static String findEditTable(String tables) {
        int spacepos = tables.indexOf(' ');
        int commapos = tables.indexOf(',');
        if (spacepos > 0 && (spacepos < commapos || commapos < 0)) return tables.substring(0, spacepos);
        if (commapos > 0 && (commapos < spacepos || spacepos < 0)) return tables.substring(0, commapos);
        return tables;
    }

    public synchronized int getVersion() {
        try (Statement s = mConnection.createStatement(); ResultSet r = s.executeQuery("PRAGMA user_version")) {
            return r.next() ? r.getInt(1) : 0;
        } catch (SQLException e) { throw new SQLiteException("version", e); }
    }

    public synchronized void setVersion(int version) { execSQL("PRAGMA user_version = " + version); }

    public synchronized void beginTransaction() {
        try {
            if (mTransactionDepth++ == 0) {
                mConnection.setAutoCommit(false);
                mTransactionFailed = false;
            }
            mSuccessful.add(false);
        } catch (SQLException e) { throw new SQLiteException("begin", e); }
    }

    public synchronized void setTransactionSuccessful() { mSuccessful.set(mSuccessful.size() - 1, true); }

    public synchronized void endTransaction() {
        boolean ok = mSuccessful.remove(mSuccessful.size() - 1);
        if (!ok) mTransactionFailed = true;
        try {
            if (--mTransactionDepth == 0) {
                if (mTransactionFailed) mConnection.rollback(); else mConnection.commit();
                mConnection.setAutoCommit(true);
            }
        } catch (SQLException e) { throw new SQLiteException("end", e); }
    }

    /**
     * Test harness only (not an android.* API): registers a SQL function that runs
     * {@code action}, so the crash harness can stop the JVM inside a statement.
     */
    public synchronized void createTestFunction(String name, Runnable action) {
        try {
            org.sqlite.Function.create(mConnection, name, new org.sqlite.Function() {
                @Override protected void xFunc() throws SQLException {
                    action.run();
                    result(0);
                }
            });
        } catch (SQLException e) { throw new SQLiteException("function " + name, e); }
    }

    public synchronized void execSQL(String sql) { execSQL(sql, new Object[0]); }

    public synchronized void execSQL(String sql, Object[] args) { executeForChanges(sql, args); }

    public synchronized void validateSql(String sql, CancellationSignal signal) {
        try (PreparedStatement statement = mConnection.prepareStatement(sql)) {
            // Preparing compiles the statement, as SQLiteDatabase.validateSql does.
        } catch (SQLException e) { throw new SQLiteException("validate " + sql, e); }
    }

    public synchronized int executeForChanges(String sql, Object[] args) {
        try (PreparedStatement statement = bind(sql, args)) {
            statement.execute();
            return statement.getUpdateCount() < 0 ? 0 : statement.getUpdateCount();
        } catch (SQLException e) { throw new SQLiteException(sql, e); }
    }

    public synchronized long executeForInsert(String sql, Object[] args) {
        try (PreparedStatement statement = bind(sql, args)) {
            statement.executeUpdate();
            try (Statement s = mConnection.createStatement(); ResultSet r = s.executeQuery("SELECT last_insert_rowid()")) {
                return r.next() ? r.getLong(1) : -1;
            }
        } catch (SQLException e) { throw new SQLiteException(sql, e); }
    }

    private PreparedStatement bind(String sql, Object[] args) throws SQLException {
        PreparedStatement statement = mConnection.prepareStatement(sql);
        if (args != null) {
            for (int i = 0; i < args.length; i++) {
                Object value = args[i];
                // SQLiteConnection binds a Boolean as 1/0.
                if (value instanceof Boolean) value = ((Boolean) value) ? 1L : 0L;
                statement.setObject(i + 1, value);
            }
        }
        return statement;
    }

    public synchronized Cursor rawQuery(String sql, String[] args) {
        return rawQueryWithFactory(null, sql, args, null, null);
    }

    public synchronized Cursor rawQueryWithFactory(CursorFactory factory, String sql,
            String[] args, String editTable, CancellationSignal signal) {
        try (PreparedStatement statement = bind(sql, args); ResultSet r = statement.executeQuery()) {
            ResultSetMetaData meta = r.getMetaData();
            String[] names = new String[meta.getColumnCount()];
            for (int i = 0; i < names.length; i++) names[i] = meta.getColumnLabel(i + 1);
            List<Object[]> rows = new ArrayList<>();
            while (r.next()) {
                Object[] row = new Object[names.length];
                for (int i = 0; i < names.length; i++) row[i] = r.getObject(i + 1);
                rows.add(row);
            }
            return new RowsCursor(names, rows);
        } catch (SQLException e) { throw new SQLiteException(sql, e); }
    }

    public synchronized long insert(String table, String nullColumnHack, ContentValues values) {
        StringBuilder sql = new StringBuilder("INSERT INTO ").append(table).append('(');
        StringBuilder marks = new StringBuilder();
        List<Object> args = new ArrayList<>();
        for (Map.Entry<String, Object> entry : values.valueSet()) {
            if (!args.isEmpty() || marks.length() > 0) { sql.append(','); marks.append(','); }
            sql.append(entry.getKey());
            marks.append('?');
            args.add(entry.getValue());
        }
        sql.append(") VALUES (").append(marks).append(')');
        try {
            return executeForInsert(sql.toString(), args.toArray());
        } catch (SQLiteException e) {
            return -1;
        }
    }

    public synchronized int update(String table, ContentValues values, String where, String[] whereArgs) {
        StringBuilder sql = new StringBuilder("UPDATE ").append(table).append(" SET ");
        List<Object> args = new ArrayList<>();
        for (Map.Entry<String, Object> entry : values.valueSet()) {
            if (!args.isEmpty()) sql.append(',');
            sql.append(entry.getKey()).append("=?");
            args.add(entry.getValue());
        }
        if (where != null && !where.isEmpty()) sql.append(" WHERE ").append(where);
        if (whereArgs != null) for (String a : whereArgs) args.add(a);
        return executeForChanges(sql.toString(), args.toArray());
    }

    public synchronized int delete(String table, String where, String[] whereArgs) {
        String sql = "DELETE FROM " + table + (where != null && !where.isEmpty() ? " WHERE " + where : "");
        return executeForChanges(sql, whereArgs);
    }

    public synchronized Cursor query(String table, String[] columns, String selection,
            String[] selectionArgs, String groupBy, String having, String orderBy) {
        return query(false, table, columns, selection, selectionArgs, groupBy, having, orderBy, null);
    }

    public synchronized Cursor query(boolean distinct, String table, String[] columns,
            String selection, String[] selectionArgs, String groupBy, String having,
            String orderBy, String limit) {
        StringBuilder sql = new StringBuilder("SELECT ");
        if (distinct) sql.append("DISTINCT ");
        sql.append(columns == null ? "*" : String.join(", ", columns)).append(" FROM ").append(table);
        if (selection != null && !selection.isEmpty()) sql.append(" WHERE ").append(selection);
        if (groupBy != null) sql.append(" GROUP BY ").append(groupBy);
        if (having != null) sql.append(" HAVING ").append(having);
        if (orderBy != null) sql.append(" ORDER BY ").append(orderBy);
        if (limit != null) sql.append(" LIMIT ").append(limit);
        return rawQuery(sql.toString(), selectionArgs);
    }

    /** A fully read result, like a filled CursorWindow. */
    static final class RowsCursor implements Cursor {
        private final String[] mNames;
        private final List<Object[]> mRows;
        private int mPosition = -1;
        RowsCursor(String[] names, List<Object[]> rows) { mNames = names; mRows = rows; }
        @Override public int getCount() { return mRows.size(); }
        @Override public boolean moveToFirst() { mPosition = 0; return !mRows.isEmpty(); }
        @Override public boolean moveToNext() { return ++mPosition < mRows.size(); }
        @Override public int getColumnIndex(String column) {
            for (int i = 0; i < mNames.length; i++) if (mNames[i].equalsIgnoreCase(column)) return i;
            return -1;
        }
        @Override public int getColumnIndexOrThrow(String column) {
            int index = getColumnIndex(column);
            if (index < 0) throw new IllegalArgumentException("column '" + column + "' does not exist");
            return index;
        }
        @Override public String[] getColumnNames() { return mNames.clone(); }
        private Object value(int column) { return mRows.get(mPosition)[column]; }
        @Override public String getString(int column) { Object v = value(column); return v == null ? null : v.toString(); }
        @Override public int getInt(int column) { Object v = value(column); return v == null ? 0 : v instanceof Number ? ((Number) v).intValue() : Integer.parseInt(v.toString()); }
        @Override public long getLong(int column) { Object v = value(column); return v == null ? 0 : v instanceof Number ? ((Number) v).longValue() : Long.parseLong(v.toString()); }
        @Override public boolean isNull(int column) { return value(column) == null; }
        @Override public void close() {}
    }
}
