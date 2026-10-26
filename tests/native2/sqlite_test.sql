-- native2 track: the sqlite3 shell (SQLite's amalgamation, built by the NDK)
-- on the bionic shim, run by tests/windows/tracks/44-native2.ps1. WAL mode
-- maps its -shm file shared and takes file locks; 20,000 rows, an index, a
-- transaction, the integrity check.
PRAGMA journal_mode=WAL;
CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, v REAL);
WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < 20000)
  INSERT INTO t SELECT x, 'name' || x, x * 0.5 FROM c;
CREATE INDEX t_name ON t(name);
SELECT count(*), printf('%.1f', sum(v)) FROM t;
SELECT id FROM t WHERE name = 'name12345';
BEGIN;
UPDATE t SET v = v * 2 WHERE id % 2 = 0;
COMMIT;
SELECT printf('%.1f', sum(v)) FROM t;
CREATE VIRTUAL TABLE docs USING fts5(body);
INSERT INTO docs VALUES ('the bridge between android and windows'), ('nothing here');
SELECT count(*) FROM docs WHERE docs MATCH 'bridge';
SELECT printf('%.4f', sqrt(2.0));
PRAGMA integrity_check;
SELECT 'sqlite_test: done';
