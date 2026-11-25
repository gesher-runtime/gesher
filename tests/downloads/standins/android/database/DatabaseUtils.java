package android.database;

import android.database.sqlite.SQLiteDatabase;

public final class DatabaseUtils {
    private DatabaseUtils() {}
    public static void appendEscapedSQLString(StringBuilder sb, String sqlString) {
        sb.append('\'');
        sb.append(sqlString.replace("'", "''"));
        sb.append('\'');
    }
    public static long executeInsert(SQLiteDatabase db, String sql, Object[] bindArgs) {
        return db.executeForInsert(sql, bindArgs);
    }
    public static int executeUpdateDelete(SQLiteDatabase db, String sql, Object[] bindArgs) {
        return db.executeForChanges(sql, bindArgs);
    }
}
