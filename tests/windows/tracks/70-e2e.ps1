# What real apps need that the test apps of the other tracks do not use
# (docs/e2e.md; the real apps themselves run in ci.yml's e2e job,
# tests/e2e/run-e2e.ps1). Dot-sourced by run-tests.ps1, whose Check, NoDrive,
# $Dir, $root and $failed these use; $root/system has framework-res.apk and
# build.prop (run-tests.ps1).

# SQLite and SharedPreferences (tests/apps/dbapp): SQLiteOpenHelper creates and
# upgrades a database file, a transaction, a Cursor over a query ordered by
# the UNICODE collator (ICU), an in-memory database, preferences committed and
# read back. gesher-app in a single process.
$bcpDb = (('core-oj', 'core-libart', 'core-icu4j', 'framework', 'gesher-app') | ForEach-Object { NoDrive "$Dir/$_.jar" }) -join ':'
$dbOut = & "$Dir/dalvikvm.exe" "-Xbootclasspath:$bcpDb" "-Ximage:$(NoDrive "$root/system")/framework/boot.art" `
  -Xint '-Xusejit:false' com.gesher.app.Main (NoDrive "$Dir/dbapp.apk") 2>&1 | Out-String
$dbExit = $LASTEXITCODE
Check 'e2e: SQLite (SQLiteOpenHelper onCreate and onUpgrade, a transaction, a Cursor, the UNICODE collator, in-memory) and SharedPreferences in dbapp.apk' {
  $global:LASTEXITCODE = $dbExit
  $dbOut
} @('DbActivity sqlite 3.50.4: onCreate onUpgrade 1->2, 4 rows, order Apple,U+00E9clair,mango,zebra, sum 100, not done 4, on disk true',
    'DbActivity in-memory: 42',
    'DbActivity prefs: committed true, name Gesher, launches 3',
    'gesher-app: done')

# Number and date formats (ICU's NumberFormat service, SimpleDateFormat), as
# list items format dates; the same run of dbapp.apk.
Check 'e2e: number and date formats (java.text and ICU NumberFormat, SimpleDateFormat, DateFormat) in dbapp.apk' {
  $global:LASTEXITCODE = $dbExit
  $dbOut
} @('DbActivity format java.text.NumberFormat: ok 42',
    'DbActivity format icu NumberFormat: ok 1,234.5',
    'DbActivity format SimpleDateFormat: ok 1970-01-01',
    'DbActivity format DateFormat.getDateFormat: ok',
    'DbActivity format icu DateFormat: ok')
