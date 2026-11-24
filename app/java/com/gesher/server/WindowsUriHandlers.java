/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.ComponentName;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.net.Uri;
import android.os.Process;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.function.Predicate;

/**
 * The Windows protocol handlers of {@code mailto:}, {@code tel:}, {@code sms:} and
 * {@code geo:} as the activities of one host package, {@code com.gesher.urihandlers}, in the
 * way {@link BrowserPackageSupport} presents the Windows default browser.
 *
 * <p>An activity exists only while Windows has a handler registered for its scheme
 * ({@code AssocQueryStringW} with {@code ASSOCF_IS_PROTOCOL}, the current user's default).
 * Without one, {@code resolveActivity} is null and {@code startActivity} throws
 * {@code ActivityNotFoundException}, as on an Android device without a mail, dialer,
 * messaging or maps app. The filters are those of AOSP's own apps for the same intents:
 * Messaging's LaunchConversationActivity (VIEW and SENDTO of sms/smsto), Dialer (VIEW and
 * DIAL of tel), a mail composer (VIEW and SENDTO of mailto) and a maps viewer (VIEW of geo).
 * Nothing of an app runs in this package: ActivityTaskManagerService hands
 * {@link #windowsUri(Intent)} to Windows' handler (WindowsHost.openUrl, ShellExecuteExW).
 *
 * <p>Not handled: ACTION_CALL (it places a call without the user's confirmation, which a
 * Windows handler does not promise), ACTION_DIAL without a number, mms/mmsto, and
 * ACTION_SEND of message/rfc822 (attachments need a MAPI client; none is offered here).
 */
public final class WindowsUriHandlers {
    public static final String PACKAGE = "com.gesher.urihandlers";
    public static final String LABEL = "Windows apps";

    public static final int VERSION_CODE = 1;
    public static final String VERSION_NAME = "1.0";

    /** One activity per Windows scheme. */
    enum Handler {
        MAIL("WindowsMailActivity", "Windows mail app", "mailto",
                new String[] { Intent.ACTION_VIEW, Intent.ACTION_SENDTO }, "mailto"),
        DIALER("WindowsDialerActivity", "Windows phone app", "tel",
                new String[] { Intent.ACTION_VIEW, Intent.ACTION_DIAL }, "tel"),
        MESSAGING("WindowsMessagingActivity", "Windows messaging app", "sms",
                new String[] { Intent.ACTION_VIEW, Intent.ACTION_SENDTO }, "sms", "smsto"),
        MAPS("WindowsMapsActivity", "Windows maps app", "geo",
                new String[] { Intent.ACTION_VIEW }, "geo");

        final String className;
        final String label;
        /** The scheme Windows is asked about and receives. */
        final String windowsScheme;
        final String[] actions;
        final String[] schemes;

        Handler(String simpleName, String label, String windowsScheme, String[] actions,
                String... schemes) {
            this.className = PACKAGE + "." + simpleName;
            this.label = label;
            this.windowsScheme = windowsScheme;
            this.actions = actions;
            this.schemes = schemes;
        }

        IntentFilter filter() {
            IntentFilter filter = new IntentFilter();
            for (String action : actions) filter.addAction(action);
            filter.addCategory(Intent.CATEGORY_DEFAULT);
            filter.addCategory(Intent.CATEGORY_BROWSABLE);
            for (String scheme : schemes) filter.addDataScheme(scheme);
            return filter;
        }

        static Handler forScheme(String scheme) {
            if (scheme == null) return null;
            String lower = scheme.toLowerCase(Locale.ROOT);
            for (Handler handler : values()) {
                for (String s : handler.schemes) {
                    if (s.equals(lower)) return handler;
                }
            }
            return null;
        }

        static Handler forClass(String className) {
            for (Handler handler : values()) {
                if (handler.className.equals(className)) return handler;
            }
            return null;
        }
    }

    /** Windows: whether a handler is registered for a URI scheme (lower case). */
    private final Predicate<String> mHasWindowsHandler;

    /**
     * @param hasWindowsHandler Windows' answer for a scheme; null when this runtime has no
     *     Windows natives, which is a device without any of these handlers
     */
    public WindowsUriHandlers(Predicate<String> hasWindowsHandler) {
        mHasWindowsHandler = hasWindowsHandler;
    }

    public static boolean isHandlerPackage(String packageName) {
        return PACKAGE.equals(packageName);
    }

    /** Whether {@code scheme} is one of the schemes this package can hand to Windows. */
    public static boolean handlesScheme(String scheme) {
        return Handler.forScheme(scheme) != null;
    }

    private boolean available(Handler handler) {
        Predicate<String> windows = mHasWindowsHandler;
        if (windows == null) return false;
        try {
            return windows.test(handler.windowsScheme);
        } catch (RuntimeException failed) {
            // A failed query is not a registered handler.
            return false;
        }
    }

    /** Whether the package exists now: Windows has a handler for at least one scheme. */
    public boolean exists() {
        for (Handler handler : Handler.values()) {
            if (available(handler)) return true;
        }
        return false;
    }

    public ApplicationInfo applicationInfo(long flags) {
        if (!exists()) return null;
        return createApplicationInfo();
    }

    private static ApplicationInfo createApplicationInfo() {
        ApplicationInfo ai = new ApplicationInfo();
        ai.packageName = PACKAGE;
        ai.nonLocalizedLabel = LABEL;
        ai.processName = PACKAGE;
        ai.flags = ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_INSTALLED;
        ai.enabled = true;
        ai.uid = Process.SYSTEM_UID;
        ai.targetSdkVersion = 34;
        ai.sourceDir = "/system/app/GesherUriHandlers.apk";
        ai.publicSourceDir = ai.sourceDir;
        ai.dataDir = "/data/data/" + PACKAGE;
        return ai;
    }

    private static ActivityInfo createActivityInfo(Handler handler) {
        ActivityInfo info = new ActivityInfo();
        info.packageName = PACKAGE;
        info.name = handler.className;
        info.nonLocalizedLabel = handler.label;
        info.processName = PACKAGE;
        info.exported = true;
        info.enabled = true;
        info.launchMode = ActivityInfo.LAUNCH_MULTIPLE;
        info.applicationInfo = createApplicationInfo();
        return info;
    }

    /** The activity {@code className} while Windows has its handler, or null. */
    public ActivityInfo activityInfo(String className, long flags) {
        Handler handler = Handler.forClass(className);
        return handler != null && available(handler) ? createActivityInfo(handler) : null;
    }

    @SuppressWarnings("deprecation") // PackageInfo.versionCode, which apps still read
    public PackageInfo packageInfo(long flags) {
        List<ActivityInfo> activities = new ArrayList<>();
        for (Handler handler : Handler.values()) {
            if (available(handler)) activities.add(createActivityInfo(handler));
        }
        if (activities.isEmpty()) return null;
        PackageInfo pi = new PackageInfo();
        pi.packageName = PACKAGE;
        pi.versionCode = VERSION_CODE;
        pi.setLongVersionCode(VERSION_CODE);
        pi.versionName = VERSION_NAME;
        pi.applicationInfo = createApplicationInfo();
        if ((flags & PackageManager.GET_ACTIVITIES) != 0) {
            pi.activities = activities.toArray(new ActivityInfo[0]);
        }
        return pi;
    }

    /** The filters of the activities that exist now, for package-visibility queries. */
    public List<IntentFilter> filters() {
        List<IntentFilter> filters = new ArrayList<>();
        for (Handler handler : Handler.values()) {
            if (available(handler)) filters.add(handler.filter());
        }
        return filters;
    }

    /**
     * The handler activity {@code intent} reaches: its explicit component, or a match of a
     * handler's filter (the package may be named), only while Windows has that handler.
     */
    public List<ResolveInfo> queryIntentActivities(Intent intent, String resolvedType, long flags) {
        List<ResolveInfo> result = new ArrayList<>();
        if (intent == null) return result;
        if (intent.getPackage() != null && !PACKAGE.equals(intent.getPackage())) return result;
        ComponentName component = intent.getComponent();
        if (component != null) {
            if (PACKAGE.equals(component.getPackageName())) {
                Handler handler = Handler.forClass(component.getClassName());
                if (handler != null && available(handler)) {
                    ResolveInfo ri = new ResolveInfo();
                    ri.activityInfo = createActivityInfo(handler);
                    result.add(ri);
                }
            }
            return result;
        }
        Handler handler = Handler.forScheme(intent.getScheme());
        if (handler == null) return result;
        IntentFilter filter = handler.filter();
        String type = resolvedType != null ? resolvedType : intent.getType();
        int match = filter.match(intent.getAction(), type, intent.getScheme(), intent.getData(),
                intent.getCategories(), "gesher");
        if (match < 0 || !available(handler)) return result;
        ResolveInfo ri = new ResolveInfo();
        ri.activityInfo = createActivityInfo(handler);
        if ((flags & PackageManager.GET_RESOLVED_FILTER) != 0) ri.filter = filter;
        ri.match = match;
        ri.isDefault = true;
        ri.nonLocalizedLabel = handler.label;
        result.add(ri);
        return result;
    }

    /**
     * The URI Windows' handler receives for {@code intent}, or null when it carries nothing
     * a handler can be given (no data). The address and content the app put in the URI and
     * in the extras its contract defines are all carried; nothing is added.
     */
    public static String windowsUri(Intent intent) {
        Uri data = intent != null ? intent.getData() : null;
        Handler handler = data != null ? Handler.forScheme(data.getScheme()) : null;
        if (handler == null) return null;
        switch (handler) {
            case MAIL: return mailto(intent, data);
            case MESSAGING: return sms(intent, data);
            case DIALER:
            case MAPS:
            default:
                // tel: and geo: carry everything in the URI (RFC 3966, RFC 5870).
                String ssp = data.getEncodedSchemeSpecificPart();
                if (ssp == null || ssp.isEmpty()) return null;
                String fragment = data.getEncodedFragment();
                return handler.windowsScheme + ":" + ssp + (fragment != null ? "#" + fragment : "");
        }
    }

    // --- mailto (RFC 6068) ----------------------------------------------------------------

    /**
     * mailto: with the URI's own fields and the extras of Intent's mail contract appended:
     * EXTRA_EMAIL to the recipients, EXTRA_CC and EXTRA_BCC to theirs, EXTRA_SUBJECT and
     * EXTRA_TEXT when the URI has no subject or body of its own.
     */
    private static String mailto(Intent intent, Uri data) {
        String ssp = data.getEncodedSchemeSpecificPart();
        if (ssp == null) ssp = "";
        int q = ssp.indexOf('?');
        String path = q >= 0 ? ssp.substring(0, q) : ssp;
        String query = q >= 0 ? ssp.substring(q + 1) : "";

        List<String> to = new ArrayList<>();
        addAddresses(to, Uri.decode(path));
        List<String> cc = new ArrayList<>();
        List<String> bcc = new ArrayList<>();
        List<String[]> other = new ArrayList<>();
        String subject = null;
        String body = null;
        if (!query.isEmpty()) {
            for (String field : query.split("&")) {
                if (field.isEmpty()) continue;
                int eq = field.indexOf('=');
                String name = Uri.decode(eq >= 0 ? field.substring(0, eq) : field);
                String value = Uri.decode(eq >= 0 ? field.substring(eq + 1) : "");
                switch (name.toLowerCase(Locale.ROOT)) {
                    case "to": addAddresses(to, value); break;
                    case "cc": addAddresses(cc, value); break;
                    case "bcc": addAddresses(bcc, value); break;
                    case "subject": if (subject == null) subject = value; break;
                    case "body": if (body == null) body = value; break;
                    default: other.add(new String[] { name, value });
                }
            }
        }
        addAll(to, stringArrayExtra(intent, Intent.EXTRA_EMAIL));
        addAll(cc, stringArrayExtra(intent, Intent.EXTRA_CC));
        addAll(bcc, stringArrayExtra(intent, Intent.EXTRA_BCC));
        if (subject == null) subject = stringExtra(intent, Intent.EXTRA_SUBJECT);
        if (body == null) body = stringExtra(intent, Intent.EXTRA_TEXT);

        StringBuilder out = new StringBuilder("mailto:");
        for (int i = 0; i < to.size(); i++) {
            if (i > 0) out.append(',');
            out.append(encode(to.get(i), "@"));
        }
        char separator = '?';
        separator = field(out, separator, "cc", join(cc));
        separator = field(out, separator, "bcc", join(bcc));
        separator = field(out, separator, "subject", subject);
        separator = field(out, separator, "body", body != null ? crlf(body) : null);
        for (String[] f : other) separator = field(out, separator, f[0], f[1]);
        return out.toString();
    }

    private static char field(StringBuilder out, char separator, String name, String value) {
        if (value == null || value.isEmpty()) return separator;
        out.append(separator).append(encode(name, "")).append('=').append(encode(value, "@,"));
        return '&';
    }

    private static void addAddresses(List<String> into, String list) {
        if (list == null) return;
        for (String address : list.split(",")) {
            String trimmed = address.trim();
            if (!trimmed.isEmpty()) into.add(trimmed);
        }
    }

    private static void addAll(List<String> into, String[] addresses) {
        if (addresses == null) return;
        for (String address : addresses) addAddresses(into, address);
    }

    private static String join(List<String> addresses) {
        return addresses.isEmpty() ? null : String.join(",", addresses);
    }

    /** RFC 6068 section 5: line breaks in a body are CRLF. */
    private static String crlf(String body) {
        return body.replace("\r\n", "\n").replace('\r', '\n').replace("\n", "\r\n");
    }

    // --- sms (RFC 5724), as AOSP Messaging's LaunchConversationActivity reads it ---------

    /**
     * sms: with the recipients of the URI (sms: or smsto:, ";" or "," separated) and the
     * body Messaging uses: the sms_body extra, else the URI's body=, else EXTRA_TEXT of a
     * text/plain intent.
     */
    private static String sms(Intent intent, Uri data) {
        String ssp = data.getEncodedSchemeSpecificPart();
        if (ssp == null) ssp = "";
        int q = ssp.indexOf('?');
        String recipients = Uri.decode(q >= 0 ? ssp.substring(0, q) : ssp).replace(';', ',');
        String body = stringExtra(intent, "sms_body");
        if (body == null || body.isEmpty()) {
            body = null;
            if (q >= 0) {
                for (String p : ssp.substring(q + 1).split("&")) {
                    if (p.startsWith("body=")) {
                        body = Uri.decode(p.substring(5));
                        break;
                    }
                }
            }
            if ((body == null || body.isEmpty()) && "text/plain".equals(intent.getType())) {
                body = stringExtra(intent, Intent.EXTRA_TEXT);
            }
        }
        StringBuilder out = new StringBuilder("sms:");
        List<String> list = new ArrayList<>();
        addAddresses(list, recipients);
        for (int i = 0; i < list.size(); i++) {
            if (i > 0) out.append(',');
            out.append(encode(list.get(i), "+"));
        }
        if (body != null && !body.isEmpty()) out.append("?body=").append(encode(body, ""));
        return out.toString();
    }

    // --- helpers ----------------------------------------------------------------------------

    /** A String[] or String extra. Bundle's typed getters log a mismatch; this reads the value. */
    @SuppressWarnings("deprecation")
    private static Object extra(Intent intent, String name) {
        android.os.Bundle extras = intent.getExtras();
        return extras != null ? extras.get(name) : null;
    }

    private static String[] stringArrayExtra(Intent intent, String name) {
        Object value = extra(intent, name);
        if (value instanceof String[]) return (String[]) value;
        if (value instanceof CharSequence) return new String[] { value.toString() };
        return null;
    }

    private static String stringExtra(Intent intent, String name) {
        Object value = extra(intent, name);
        return value instanceof CharSequence ? value.toString() : null;
    }

    /** UTF-8 percent-encoding of all but RFC 3986 unreserved characters and {@code keep}. */
    static String encode(String s, String keep) {
        StringBuilder out = new StringBuilder();
        byte[] bytes = s.getBytes(java.nio.charset.StandardCharsets.UTF_8);
        for (byte b : bytes) {
            int c = b & 0xff;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                    || c == '-' || c == '.' || c == '_' || c == '~'
                    || (c < 0x80 && keep.indexOf(c) >= 0)) {
                out.append((char) c);
            } else {
                out.append('%').append(Character.toUpperCase(Character.forDigit(c >> 4, 16)))
                        .append(Character.toUpperCase(Character.forDigit(c & 0xf, 16)));
            }
        }
        return out.toString();
    }
}
