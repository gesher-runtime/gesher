# Gesher — yom-chadash QA handoff

תאריך: 17 בנובמבר 2026. החיבור שנבדק: palmoni5 בלבד.

## היקף ואמינות הדיווח

נסרקו 14 PRים פתוחים שאינם טיוטה: 85, 87, 92, 93, 94, 95, 96, 97,
100, 101, 102, 104, 105, 106. זו אינה הצהרת QA מלאה או אישור לכל ה־PRים.
בבדיקה הנוכחית לא הופעלו סוכני משנה ולא בוצעה כתיבה ל־GitHub: הכלים
הזמינים היו לקריאה בלבד. סקירות וענפי תיקון קיימים שנמצאו במאגר מובחנים
להלן מהבדיקות המקומיות שבוצעו כאן.

לא נוצרו קומיטים, תוויות, סקירות או מיזוגים בחיבור GitHub בסבב הנוכחי.
הסקריפט המצורף מאפשר למשתמש להחיל קומיטי תיקון קיימים שכבר פורסמו על
ידי palmoni5. הוא אינו עוקף QA או CI ואינו מאשר PRים.

## תוצאות בעלות ראיות

### #97 — תיקון שכבר נקלט

הראש שנבדק: `6d43bf853b3e5467947dcee31d6f8ee947ff4f6b`.
התיקון ל־O_PATH כבר נמצא ב־PR. נמצאו סקירות APPROVED המתייחסות לראש הזה
וגם תווית automerge. ה־PR עדיין פתוח ולא ממוזג. בבדיקת ה־CI האחרונה,
ci run 44115457934 היה בתור ו־arm64-windows 44115457610 היה בריצה.
תוצאות אלה אינן הצלחת CI.

הבדיקות הרבות שמפורטות בסקירות GitHub הן דיווחי הסבב המקביל; הן לא
הורצו מחדש בבדיקה המקומית המתועדת בחבילה הזאת.

מקורות:
- https://github.com/gesher-runtime/gesher/pull/97
- https://github.com/gesher-runtime/gesher/pull/97#pullrequestreview-5941639533
- https://github.com/gesher-runtime/gesher/pull/97#pullrequestreview-5941827330

### #87 — תגובה חסרה חייבת להישאר חסרה

הראש המקורי: `c0b2a744dfa1be80eabd22cfb29ea99d20656b23`.
הקוד בונה Response עם קוד HTTP אפס עוד לפני חיבור, ומוסר אותו גם בכשל
מוקדם ובביטול. הבדיקה החדשה של ה־PR מצפה לאובייקט הזה. ב־Cronet המידע
נוצר אחרי קבלת כותרות תגובה, והוא null לפני כן.

בבדיקה הנוכחית נבנתה והורצה המחלקה המלאה JavaBidirectionalStream מתוך
הקוד שנשלף מהמאגר, מול תחליפי ממשקי framework/transport/executor לצורכי
בדיקה בלבד. זוהו ארבע חריגות מתוך 18 בדיקות: כשל כאשר HTTP/2 כבוי,
כשל חיבור, ביטול לפני I/O, ו־write callback לפני תגובה.

העותק המקומי אומת באמצעות מזהה Git blob מול המזהה שהחזיר GitHub:
- קוד הייצור המקורי: `18c9d3632c36e83880d39546b2a52d2c0768ad77`.
- קוד הייצור מהתיקון הקיים: `c20cd0c774aef3a40812f972145d1e000f82002d`.

נמצא במקביל תיקון רחב יותר שכבר הוכן:
`503eaf20885481c4f860d9bde611c75a82823448`, בענף
`qa/pr-87-http2-policy-20261117`. הוא מטפל גם בהפרדת כותרות HTTP רגילות
מכותרות הבקרה הפרטיות של OkHttp. לא הוכן לו קומיט מתחרה.
המחלקה המדויקת מהתיקון הזה הורצה כאן ועברה את כל 18 הבדיקות.

הבדיקה המקומית אינה בדיקת פרוטוקול TLS/HTTP2 אמיתי, אינה ART או Windows,
ואינה מבחן מרוצים/עומס. היא אינה מאשרת לבדה את כל התיקון.

מקורות:
- https://github.com/gesher-runtime/gesher/pull/87#issuecomment-6952967701
- https://github.com/gesher-runtime/gesher/commit/503eaf20885481c4f860d9bde611c75a82823448
- https://android.googlesource.com/platform/external/cronet/+/e05501550b3b70236b6c53f77befe65e1f87020f/components/cronet/android/java/src/org/chromium/net/impl/CronetBidirectionalStream.java
- https://developer.android.com/develop/connectivity/cronet/reference/org/chromium/net/BidirectionalStream.Callback

### #92 — קבלת בקשת פתיחה אינה סיום Activity

הראש שנבדק: `80265b33640c046b3b7147ff6e65ba082e8991b1`.
נמצא תיקון קיים `09627a9a1964ceb863a8eb46d33b458b11db6a00`, בענף
`qa/pr-92-external-url-results-20261117`. הוא מתקן את החזרת התוצאה המוקדמת
ואת זהות ה־caller בעת ביטול הפעלה. לפי סקירת הסבב המקביל, דיווח הצלחה
על פתיחת כתובת אינו רשאי להיחשב לסיום הפעילות החיצונית. התיקון עוד לא
נקלט בראש ה־PR בזמן האימות.

מקור:
https://github.com/gesher-runtime/gesher/pull/92#issuecomment-6952707730

### #93 — כשל fixture נפרד מחוסר מהותי בניהול נעילות

הראש שנבדק: `1df606ebdc780bc376156e63c2e3a96fef350138`.
אומת כשל ci run 44086341476. בלוג artifact-preflight נראתה שגיאת
ForEachWorkingSetBatch שאינו מוגדר בקוד ה־host שנוצר. בזמן הבירור חלק
מנתיבי הקבצים המבוקשים החזירו 404; לא הוחל patch על תוכן שלא התקבל.

נמצא בהמשך תיקון קיים בשם
`4e53dd7a46191d75df1bd1914389f599fe4b1828`, בענף
`qa/pr-93-working-set-fixture-20261117`. תיקון זה מוסיף את חילוץ פונקציית
העזר ב־tools/test_mman_zeroing_mappings.py ושומר את הבדיקות הקיימות.
ה־diff שלו נקרא; בדיקותיו לא הורצו מחדש כאן.

גם לאחר החלתו נותרת בקשת שינויים מהותית, שכבר פורסמה על הראש המקורי:
אין חשבון נעילות תואם RLIMIT_MEMLOCK, munlockall אינו משחרר נעילות
רגילות, והעברת mapping דרך mremap אינה שומרת את מצב הנעילה.
תיקון fixture אינו פותר את ניהול מחזור החיים הזה ואינו מהווה אישור.

מקורות:
- https://github.com/gesher-runtime/gesher/actions/runs/44086341476/job/134065643726
- https://github.com/gesher-runtime/gesher/commit/4e53dd7a46191d75df1bd1914389f599fe4b1828
- https://github.com/gesher-runtime/gesher/pull/93#pullrequestreview-5941741859

### #94 — חותמת זמן מפורשת אינה AUTO

הראש שנבדק: `cc7656eaeb0b32b1b81d6bd264b524de1ebec00f`.
נמצא תיקון קיים `78730c164966b0a288e9b65ae0b7e9ca5612a335`, בענף
`qa/pr-94-explicit-timestamps-20261117`. הוא מבדיל בין חותמת זמן מפורשת
לפריים לבין הגדרת AUTO המתמשכת של ה־native window, כולל INT64_MIN.
נקרא ה־diff של התיקון ושל הבדיקות שלו. לא בוצעה כאן הרצת מדיה או Windows.

מקור:
https://github.com/gesher-runtime/gesher/commit/78730c164966b0a288e9b65ae0b7e9ca5612a335

### #96 — הרשאת גילוי אינה השוואת שם חבילה

הראש שנבדק מחדש: `dbb41d004c72d9debdd1d689509056e0c80c7180`.
נקראה המחלקה SearchManagerService והושוותה למקור Android16-release של
Searchables. הקוד שולל באופן גורף חיפוש מטא־דאטה מחבילה אחרת. AOSP
משתמש ב־canAccessComponent לפי UID, רכיב ומשתמש, ולא בשוויון שם החבילה.

נדרש שילוב עם מדיניות הנראות האמיתית, גם לתוצאה ישירה וגם להפניה.
אין להסיר את התנאי ולחשוף מפה לא מסוננת. יש לבדוק רכיב זר נגיש לעומת
חבילה מוסתרת, target אסור, הפניית default_searchable, ו־disable/removal
בזמן פענוח. זו מסקנת source QA; לא הורץ כאן Binder/ART של ה־PR.

בבדיקה החוזרת נמצאה בקשת CHANGES_REQUESTED שכבר פורסמה על אותו SHA,
ולכן אין צורך לפרסם טיוטה כפולה מהחבילה הזאת.

מקורות:
- https://github.com/gesher-runtime/gesher/blob/dbb41d004c72d9debdd1d689509056e0c80c7180/app/java/com/gesher/server/SearchManagerService.java
- https://github.com/aosp-mirror/platform_frameworks_base/blob/android16-release/services/core/java/com/android/server/search/Searchables.java
- https://github.com/gesher-runtime/gesher/pull/96#pullrequestreview-5941941461

## PRים שהסקירה שלהם עדיין אינה QA מלאה

85, 95, 100, 101, 102, 104, 105, 106. נקראו diffs ומידע שנשלף בסבב,
אבל לא הושלמו עבורם כל בדיקות ההתנהגות, הביצועים, מחזור החיים ו־CI
הנחוצות לאישור. אין לפרש היעדר ממצא נוסף כסימן שאושרו.

## החלת ארבעת התיקונים שכבר מוכנים

הסקריפט Apply-YomRepairs.ps1 נועד ל־PowerShell 5.1. הוא נבדק בקריאת קוד,
אך לא הורץ כאן ב־PowerShell ולא ביצע כאן פעולת GitHub.

מתוך התיקייה שאליה חולצה החבילה:

```powershell
.\Apply-YomRepairs.ps1 -RepositoryPath 'C:\Users\otzaria\Documents\gesher'
```

זהו שלב אימות בלבד. כדי להחיל:

```powershell
.\Apply-YomRepairs.ps1 -RepositoryPath 'C:\Users\otzaria\Documents\gesher' -Apply
```

הסקריפט מאמת palmoni5, את מצב ה־PR, שאינו טיוטה, את הפורק והענף,
את ה־SHA המדויק ואת היות קומיט התיקון צאצא של הראש הנבדק. הוא דוחף
fast-forward בלבד, בלי checkout או שינוי קובצי העבודה. ענף זמני נמחק
רק אחרי אימות העדכון ורק באמצעות lease על הגרסה המדויקת שלו.

אם התורם פרסם ראש חדש, הסקריפט עוצר. אין להוסיף force לדחיפה לפורק.
אם העדכון נכשל באמצע הסבב, הסקריפט מדווח אילו PRים כבר עודכנו.
אין rollback אוטומטי או כתיבה מחדש של היסטוריית התורם.
הסקריפט אינו מטפל בקונפליטים חדשים, אינו מאשר, אינו משנה תגיות ואינו
ממזג ל־main. לאחריו צריך QA לראש החדש ו־CI של אותו ראש.

## הרצת הבדיקה המקומית הנוספת ל־#87

עם JDK 17 ומעלה ו־Python:

```powershell
python .\tests\test_pr87_response_presence.py --source 'C:\path\to\gesher\app\java\android\net\http\JavaBidirectionalStream.java'
```

זו בדיקה מקומית מבודדת. היא לא משנה את קובץ המקור. היא מדפיסה את ה־Git
blob של המקור שהיא מקמפלת, כדי להפריד במפורש בין גרסאות. הלוגים שלפני
התיקון ושל הקובץ מתוך התיקון 503eaf2 כלולים בתיקיית tests.
