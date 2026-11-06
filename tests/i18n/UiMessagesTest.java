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
