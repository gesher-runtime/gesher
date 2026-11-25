import com.gesher.app.locale.UiMessages;

public class UiMessagesTest {
    static void equal(Object expected, Object actual) {
        if (!expected.equals(actual)) throw new AssertionError(expected + " != " + actual);
    }
    public static void main(String[] args) {
        equal("he", UiMessages.language("he-IL-u-ca-hebrew,en-US"));
        equal("he", UiMessages.language(" HE_il , en-US"));
        equal("he", UiMessages.language("de-DE,iw-IL,en-US"));
        equal("en", UiMessages.language("en-GB,he-IL"));
        equal("en", UiMessages.language("ar-SA,fr-FR"));
        equal("en", UiMessages.language(null));
        equal("en", UiMessages.language("hebrew,xx"));
        equal(true, UiMessages.isRtl("iw-IL"));
        equal(false, UiMessages.isRtl("en-US,he-IL"));
        equal("Starting…", UiMessages.text("starting", "en-US"));
        equal("4 installed packages", UiMessages.text("package_count", "en-US", 4));
        equal("מפעיל…", UiMessages.text("starting", "he-IL"));
        equal("קובץ ההרחבה אינו קיים: C:\\שלום\\{1}.apk",
                UiMessages.text("microg_missing", "he-IL", "C:\\שלום\\{1}.apk"));
        equal("proxy of name{1}: direct", UiMessages.text("proxy_launch", "en", "name{1}", "direct", ""));
        for (String key : new String[]{"install_request", "removal_request"}) {
            String packageName = "com.android.vending";
            int version = 85430740;
            String english = UiMessages.text(key, "en", packageName, version);
            String hebrew = UiMessages.text(key, "he", packageName, version);
            // The Win32 EDIT control displays isolate characters as boxes and expects CRLF.
            equal(true, english.contains(packageName + " (version " + version + ")?\r\n"));
            equal(true, hebrew.contains("\u202aAndroid " + packageName + "\u202c"));
            equal(true, hebrew.contains("\u202a" + version + "\u202c)?\r\n"));
            equal(false, (english + hebrew).chars().anyMatch(c -> c >= 0x2066 && c <= 0x2069));
        }
        String enNoAbi = UiMessages.text("install_no_matching_abis", "en", "app.apk", "armeabi-v7a");
        equal(true, enNoAbi.contains("app.apk"));
        equal(true, enNoAbi.contains("armeabi-v7a"));
        equal(true, enNoAbi.contains("arm64-v8a"));
        String heNoAbi = UiMessages.text("install_no_matching_abis", "he", "מוביט.apk", "armeabi-v7a");
        equal(true, heNoAbi.contains("מוביט.apk"));
        equal(true, heNoAbi.contains("armeabi-v7a"));
        equal(true, heNoAbi.contains("arm64-v8a"));
        equal(true, heNoAbi.contains("\u202aמוביט.apk\u202c"));
        try {
            UiMessages.text("unknown", "en");
            throw new AssertionError("Unknown keys must fail");
        } catch (IllegalArgumentException expected) { }
        try {
            UiMessages.text("microg_missing", "he");
            throw new AssertionError("Missing arguments must fail");
        } catch (IllegalArgumentException expected) { }
        System.out.println("PASS: UI language selection, Hebrew, RTL, fallback and literal arguments");
    }
}
