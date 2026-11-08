/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app.shell;

import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;

import com.android.internal.pm.pkg.component.ParsedActivity;
import com.android.internal.pm.pkg.component.ParsedIntentInfo;
import com.gesher.app.Main;
import static com.gesher.app.locale.UiText.text;
import com.gesher.app.locale.UiText;
import com.gesher.app.locale.UiMessages;
import com.gesher.server.ApkPackage;
import com.gesher.server.Device;
import com.gesher.server.Zygote;

import java.io.File;
import java.util.Iterator;
import java.util.LinkedHashSet;
import java.util.Locale;
import java.util.Properties;
import java.util.Set;

/**
 * Installed apps as Windows apps (docs/installer.md, ADR 0028): each package with a launcher
 * activity gets a Start menu shortcut, {@code <Programs>\Gesher Apps\<label>.lnk}, that runs
 * {@code gesher-app launch <package>}, with the app's label and icon ({@link AppIcon}, a .ico
 * in the package's data directory) and an AppUserModelID of its own, {@link
 * #appUserModelId}; the system server gives the package's processes the same ID ({@link
 * #ENV_APP_USER_MODEL_ID}), so their windows group under the shortcut on the taskbar.
 *
 * <p>{@code gesher-app install} and launches make or update the shortcut ({@link
 * #registerQuietly}), {@code gesher-app uninstall} removes it ({@link #unregisterQuietly});
 * uninstalling Gesher removes the whole folder (gesher.iss). What a shortcut belongs to is
 * recorded in {@code <data dir>/gesher-shell/shortcut.properties}, next to {@code icon.ico}.
 *
 * <p>The custom schemes of a package's BROWSABLE VIEW filters (an OAuth redirect, as AppAuth's
 * RedirectUriReceiverActivity declares one) become per-user URL protocols that run {@code
 * gesher-launch view <package> "%1"} ({@link #registerUrlProtocolsQuietly}): a URL the Windows
 * browser opens comes back to the app as ACTION_VIEW. They are recorded in {@code
 * url-protocols.properties} and removed with the package ({@link
 * #unregisterUrlProtocolsQuietly}).
 */
public final class AppShell {
    /** The Start menu folder of the apps, under the user's Programs folder. */
    public static final String FOLDER = text("apps_folder");
    /**
     * The AppUserModelID an app process sets for itself (SetCurrentProcessExplicitAppUserModelID,
     * {@link #applyProcessAppUserModelId}): the system server puts it in the environment of the
     * processes it starts for a package.
     */
    public static final String ENV_APP_USER_MODEL_ID = "GESHER_APP_USER_MODEL_ID";
    /** {@code GESHER_SHORTCUTS=0}: no Start menu shortcuts (and no URL protocols). */
    public static final String ENV_SHORTCUTS = "GESHER_SHORTCUTS";

    private static final String TAG = "gesher-app";
    private static final String RECORD = "shortcut.properties";
    private static final String ICON = "icon.ico";

    private AppShell() {
    }

    private static void log(String message) {
        System.out.println(TAG + ": " + message);
        System.out.flush();
    }

    /**
     * The package's AppUserModelID: {@code Gesher.App.<package>} (at most 128 characters, as
     * Windows requires; a longer one ends with a hash of the package name).
     */
    public static String appUserModelId(String packageName) {
        String id = "Gesher.App." + packageName;
        if (id.length() > 128) {
            id = id.substring(0, 119) + "." + String.format("%08x", packageName.hashCode());
        }
        return id;
    }

    /**
     * In an app process: sets the process's AppUserModelID from {@link #ENV_APP_USER_MODEL_ID},
     * before it shows a window, and logs it. Does nothing without it.
     */
    public static void applyProcessAppUserModelId(String tag) {
        String id = System.getenv(ENV_APP_USER_MODEL_ID);
        if (id == null || id.isEmpty()) return;
        try {
            int hr = Shell.setProcessAppUserModelId(id);
            System.out.println(tag + ": Windows AppUserModelID " + Shell.processAppUserModelId()
                    + (hr != 0 ? String.format(" (HRESULT 0x%08x)", hr) : ""));
        } catch (Throwable t) {
            System.out.println(tag + ": Windows AppUserModelID " + id + " not set: " + t);
        }
        System.out.flush();
    }

    // --- shortcuts ------------------------------------------------------------------------

    private static File shellDir(String packageName) {
        return new File(Device.dataDir(), "data/" + packageName + "/gesher-shell");
    }

    /** The shortcut recorded for the package, or null. */
    public static Properties shortcutOf(String packageName) {
        try {
            return Device.read(new File(shellDir(packageName), RECORD));
        } catch (Throwable t) {
            return null;
        }
    }

    /**
     * After {@code packageName} was installed or updated ({@code changed}), or found installed,
     * with its code in {@code codePath}: makes or updates its Start menu shortcut, unless it is
     * up to date. Logs what it did; never throws.
     */
    public static void registerQuietly(String packageName, String codePath, boolean changed) {
        try {
            register(packageName, codePath, changed);
        } catch (Throwable t) {
            log("no Start menu shortcut for " + packageName + ": " + t);
        }
    }

    private static void register(String packageName, String codePath, boolean changed)
            throws Exception {
        if ("0".equals(System.getenv(ENV_SHORTCUTS))) return;
        String gesher = installDir();
        String systemRoot = System.getenv("SystemRoot");
        if (systemRoot == null || systemRoot.isEmpty()) systemRoot = "C:\\Windows";
        String psExe = systemRoot + "\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
        String psScript = gesher + "\\gesher-app.ps1";
        // gesher-launch.exe runs gesher-app.ps1 with no console window and lets the app's
        // window come to the front (compat/shell/gesher_launch.cc); an older Gesher has none.
        String launchExe = gesher + "\\gesher-launch.exe";
        boolean viaLauncher = Shell.exists(launchExe);
        String kind = viaLauncher ? "launcher" : "powershell";
        String target = viaLauncher ? launchExe : Shell.exists(psExe) ? psExe : "powershell.exe";
        if (!Shell.exists(psScript)) {
            log("no Start menu shortcut for " + packageName + ": no " + psScript
                    + " (not an installed Gesher)");
            return;
        }
        String programs = Shell.programsFolder();
        if (programs == null) {
            log("no Start menu shortcut for " + packageName + ": no Programs folder");
            return;
        }
        Properties old = shortcutOf(packageName);
        String oldLink = old != null ? old.getProperty("link") : null;
        String oldTarget = old != null ? old.getProperty("target") : null;
        if (!changed && oldLink != null && codePath.equals(old.getProperty("code"))
                && Shell.exists(oldLink) && kind.equals(oldTarget)
                && UiText.language().equals(old.getProperty("ui_language"))) {
            return;  // up to date
        }

        ApkPackage pkg = ApkPackage.parse(codePath + "/base.apk");
        ActivityInfo launcher = launcherActivity(pkg);
        if (launcher == null) {
            // As a launcher's app list: only apps with a MAIN/LAUNCHER activity.
            log(packageName + " has no launcher activity: no Start menu shortcut");
            removeShortcut(old);
            return;
        }
        AppIcon icon = AppIcon.load(pkg.apkPath, pkg.applicationInfo(0), launcher);
        File dir = shellDir(packageName);
        File ico = new File(dir, ICON);
        String iconPath;
        if (icon.hasImages()) {
            icon.writeIco(ico);
            iconPath = Zygote.windowsPath(ico.getPath());
        } else {
            iconPath = gesher + "\\gesher.ico";
            if (!Shell.exists(iconPath)) iconPath = "";
        }
        String folder = programs + "\\" + FOLDER;
        if (!Shell.makeDirectories(folder)) throw new IllegalStateException("cannot create " + folder);
        String link = linkPath(folder, icon.label, packageName, oldLink);
        String id = appUserModelId(packageName);
        String arguments = viaLauncher ? "launch " + packageName
                : "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + psScript
                        + "\" launch " + packageName;
        int hr = Shell.createShortcut(link, target, arguments, gesher, iconPath,
                text("run_app", icon.label + " (" + packageName + ")"), id);
        if (hr != 0) {
            throw new IllegalStateException(String.format("saving %s failed: HRESULT 0x%08x",
                    link, hr));
        }
        if (oldLink != null && !oldLink.equalsIgnoreCase(link)) removeShortcut(old);
        Properties record = new Properties();
        record.setProperty("package", packageName);
        record.setProperty("link", link);
        record.setProperty("label", icon.label);
        record.setProperty("icon", iconPath);
        record.setProperty("code", codePath);
        record.setProperty("app_user_model_id", id);
        record.setProperty("target", kind);
        record.setProperty("ui_language", UiText.language());
        dir.mkdirs();
        Device.write(new File(dir, RECORD), record);
        log("Start menu shortcut " + link + ": " + icon.label + ", AppUserModelID " + id
                + ", icon " + (icon.hasImages() ? iconPath : "Gesher's") + " from "
                + icon.source);
    }

    /**
     * Before uninstalling {@code packageName}, {@link #shortcutOf} it; after, this removes the
     * shortcut, the icon, and the folder if it is empty. Logs what it did; never throws.
     */
    public static void unregisterQuietly(String packageName, Properties shortcut) {
        try {
            removeShortcut(shortcut);
            File dir = shellDir(packageName);
            new File(dir, ICON).delete();
            new File(dir, RECORD).delete();
            java.nio.file.Files.deleteIfExists(dir.toPath());
            rewriteApkIcons(packageName, null);
        } catch (Throwable t) {
            log("the Start menu shortcut of " + packageName + " was not removed: " + t);
        }
    }

    // --- URL protocols -------------------------------------------------------------------

    private static final String PROTOCOLS = "url-protocols.properties";

    /**
     * Schemes that are never an app's own on Windows, whatever its filters say: the web's,
     * mail's, files' and Android's own (Windows' browser, mail and phone apps open them).
     */
    private static final Set<String> SYSTEM_SCHEMES = Set.of("http", "https", "mailto",
            "file", "ftp", "content", "android.resource", "intent", "javascript", "data",
            "about", "blob", "ws", "wss", "tel", "sms", "smsto", "mms", "mmsto", "geo", "market",
            "android-app");

    /** The URL protocols recorded for the package, or null. */
    public static Properties urlProtocolsOf(String packageName) {
        try {
            return Device.read(new File(shellDir(packageName), PROTOCOLS));
        } catch (Throwable t) {
            return null;
        }
    }

    /**
     * The custom schemes of the package's BROWSABLE VIEW filters, lower case, as Windows
     * names URL protocols: a letter, then letters, digits, '+', '-' or '.' (at least two
     * characters: one is a drive letter). Not {@link #SYSTEM_SCHEMES}.
     */
    static Set<String> customSchemes(ApkPackage pkg) {
        Set<String> schemes = new LinkedHashSet<>();
        for (ParsedActivity a : pkg.parsed.getActivities()) {
            for (ParsedIntentInfo info : a.getIntents()) {
                IntentFilter filter = info.getIntentFilter();
                if (!filter.hasAction(Intent.ACTION_VIEW)
                        || !filter.hasCategory(Intent.CATEGORY_BROWSABLE)) {
                    continue;
                }
                Iterator<String> it = filter.schemesIterator();
                while (it != null && it.hasNext()) {
                    String scheme = it.next().toLowerCase(Locale.ROOT);
                    if (scheme.matches("[a-z][a-z0-9+.-]+") && !SYSTEM_SCHEMES.contains(scheme)) {
                        schemes.add(scheme);
                    }
                }
            }
        }
        return schemes;
    }

    /**
     * After {@code packageName} was installed or updated ({@code changed}), or found installed,
     * with its code in {@code codePath}: registers the URL protocols of its custom schemes
     * ({@link #customSchemes}) and removes those it no longer declares, unless its record is
     * up to date. A scheme another program (or another package) has is left to it. Logs
     * what it did; never throws.
     */
    public static void registerUrlProtocolsQuietly(String packageName, String codePath,
            boolean changed) {
        try {
            registerUrlProtocols(packageName, codePath, changed);
        } catch (Throwable t) {
            log("no URL protocols for " + packageName + ": " + t);
        }
    }

    private static void registerUrlProtocols(String packageName, String codePath,
            boolean changed) throws Exception {
        if ("0".equals(System.getenv(ENV_SHORTCUTS))) return;
        String gesher = installDir();
        String systemRoot = System.getenv("SystemRoot");
        if (systemRoot == null || systemRoot.isEmpty()) systemRoot = "C:\\Windows";
        String psExe = systemRoot + "\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
        String psScript = gesher + "\\gesher-app.ps1";
        String launchExe = gesher + "\\gesher-launch.exe";
        if (!Shell.exists(psScript)) return;  // not an installed Gesher (register says so)
        // The shortcut's way (register): gesher-launch.exe, else powershell.
        boolean viaLauncher = Shell.exists(launchExe);
        String kind = viaLauncher ? "launcher" : "powershell";
        Properties old = urlProtocolsOf(packageName);
        if (!changed && old != null && codePath.equals(old.getProperty("code"))
                && kind.equals(old.getProperty("target"))
                && UiText.language().equals(old.getProperty("ui_language"))) {
            return;  // up to date
        }
        Set<String> schemes = customSchemes(ApkPackage.parse(codePath + "/base.apk"));
        String command = viaLauncher
                ? "\"" + launchExe + "\" view " + packageName + " \"%1\""
                : "\"" + (Shell.exists(psExe) ? psExe : "powershell.exe")
                        + "\" -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \""
                        + psScript + "\" view " + packageName + " \"%1\"";
        File ico = new File(shellDir(packageName), ICON);
        String icon = ico.isFile() ? Zygote.windowsPath(ico.getPath()) : gesher + "\\gesher.ico";
        Set<String> registered = new LinkedHashSet<>();
        for (String scheme : schemes) {
            int hr = Shell.registerUrlProtocol(scheme, packageName,
                    text("url_protocol", scheme, packageName),
                    command, icon);
            if (hr == 0) {
                registered.add(scheme);
                log("URL protocol " + scheme + ": " + command);
            } else if (hr == Shell.ALREADY_EXISTS) {
                String owner = Shell.urlProtocolOwner(scheme);
                log("URL protocol " + scheme + ": not registered for " + packageName + ", it is "
                        + (owner != null ? owner + "'s" : "another program's"));
            } else {
                log(String.format("URL protocol %s: not registered for %s: HRESULT 0x%08x",
                        scheme, packageName, hr));
            }
        }
        // Those the package no longer declares (an update).
        String oldSchemes = old != null ? old.getProperty("schemes", "") : "";
        for (String scheme : oldSchemes.split(",")) {
            if (!scheme.isEmpty() && !registered.contains(scheme)
                    && Shell.unregisterUrlProtocol(scheme, packageName) == 0) {
                log("URL protocol " + scheme + ": removed (" + packageName
                        + " no longer declares it)");
            }
        }
        Properties record = new Properties();
        record.setProperty("package", packageName);
        record.setProperty("code", codePath);
        record.setProperty("target", kind);
        record.setProperty("schemes", String.join(",", registered));
        record.setProperty("ui_language", UiText.language());
        File dir = shellDir(packageName);
        dir.mkdirs();
        Device.write(new File(dir, PROTOCOLS), record);
    }

    /**
     * Before uninstalling {@code packageName}, {@link #urlProtocolsOf} it; after, this removes
     * its URL protocols (those still its own). Logs what it did; never throws.
     */
    public static void unregisterUrlProtocolsQuietly(String packageName, Properties record) {
        try {
            if (record == null) return;
            for (String scheme : record.getProperty("schemes", "").split(",")) {
                if (scheme.isEmpty()) continue;
                int hr = Shell.unregisterUrlProtocol(scheme, packageName);
                if (hr == 0) {
                    log("removed the URL protocol " + scheme + " of " + packageName);
                } else if (hr != 1) {  // 1 (S_FALSE): no longer its own
                    log(String.format("the URL protocol %s of %s was not removed: HRESULT 0x%08x",
                            scheme, packageName, hr));
                }
            }
            new File(shellDir(packageName), PROTOCOLS).delete();
        } catch (Throwable t) {
            log("the URL protocols of " + packageName + " were not removed: " + t);
        }
    }

    // --- the .apk files' icons ---------------------------------------------------------

    /**
     * The APK files Gesher installed, for the .apk file type's icon handler
     * (compat/shell/gesher_shellext.cc): a line per file, its size, its last write time
     * (milliseconds), the app's .ico (a Windows path) and the package, tab-separated.
     */
    private static final String APK_ICONS = "system/apk-icons.list";
    /** Lines kept: the most recent files. */
    private static final int APK_ICONS_KEPT = 256;

    /**
     * After {@code packageName} was installed from {@code apk} (and {@link #registerQuietly}
     * made its icon): Explorer shows that file with the app's icon. Never throws.
     */
    public static void rememberApkQuietly(String packageName, File apk) {
        try {
            File ico = new File(shellDir(packageName), ICON);
            if (!ico.isFile()) return;  // no icon of its own: Gesher's, as any other APK
            String line = apk.length() + "\t" + apk.lastModified() + "\t"
                    + Zygote.windowsPath(ico.getPath()) + "\t" + packageName;
            rewriteApkIcons(null, line);
            Shell.notifyChanged(Zygote.windowsPath(apk.getAbsolutePath()));
        } catch (Throwable t) {
            log("no Explorer icon for " + apk + ": " + t);
        }
    }

    /**
     * The icons {@link #apkIcon} drew for APK files of apps not installed: {@code
     * <size>-<time>.ico}, as long as a line of the list names them.
     */
    private static final String APK_ICON_CACHE = "system/apk-icons";
    /** A drawn icon no line names is removed once it is this old (ms). */
    private static final long APK_ICON_CACHE_GRACE_MS = 10 * 60_000L;

    /**
     * {@code gesher-app apk-icon APK}, also accepting APKS/XAPK bundles, what the icon
     * handler runs for a file it has no
     * icon of (gesher_shellext.dll, in the background, at most twice at once): draws the
     * app's icon as {@link #registerQuietly} does, without installing the app, into
     * {@code <data>/system/apk-icons/<size>-<time>.ico}, records the file in the list, and
     * tells the shell the file changed. A file that is no APK, or whose icon cannot be drawn,
     * is recorded with Gesher's icon, so it is not tried again until it changes. Returns the
     * exit code: 0 when the file is recorded.
     */
    public static int apkIcon(File apk) {
        long size = apk.length();
        long time = apk.lastModified();
        if (!apk.isFile() || size == 0) {
            apkIconLog(apk + ": no such file, or empty");
            return 1;
        }
        String windowsApk = Zygote.windowsPath(apk.getAbsolutePath());
        String iconPath = null;
        String packageName = "-";
        String what;
        try (IconApks inputs = IconApks.open(apk)) {
            ApkPackage pkg = ApkPackage.parse(inputs.base);
            packageName = pkg.packageName;
            AppIcon icon = AppIcon.load(inputs.paths, pkg.applicationInfo(0),
                    launcherActivity(pkg));
            if (icon.hasImages()) {
                File ico = new File(new File(Device.dataDir(), APK_ICON_CACHE),
                        size + "-" + time + ".ico");
                icon.writeIco(ico);
                iconPath = Zygote.windowsPath(ico.getPath());
                what = packageName + " (" + icon.label + "): " + iconPath + " from "
                        + icon.source;
            } else {
                what = packageName + ": no icon drawn (" + icon.source + ")";
            }
        } catch (Throwable t) {
            what = "not read: " + t;
        }
        if (apk.length() != size || apk.lastModified() != time) {
            // Changed meanwhile: the handler asks again for the file as it is now.
            apkIconLog(windowsApk + ": changed while it was read; " + what);
            return 1;
        }
        if (iconPath == null) {
            iconPath = installDir() + "\\gesher.ico";
            what += "; Gesher's icon";
        }
        try {
            rewriteApkIcons(null, size + "\t" + time + "\t" + iconPath + "\t" + packageName);
        } catch (Throwable t) {
            apkIconLog(windowsApk + ": " + what + "; not recorded: " + t);
            return 1;
        }
        try {
            Shell.notifyChanged(windowsApk);
        } catch (Throwable t) {
            // The handler tells the shell too.
        }
        apkIconLog(windowsApk + ": " + what);
        return 0;
    }

    /** A line in {@code <logs>/apk-icons.log} (and on the output); never throws. */
    private static void apkIconLog(String message) {
        log("apk-icon: " + message);
        try {
            File file = new File(Device.logsDir(), "apk-icons.log");
            file.getParentFile().mkdirs();
            if (file.length() > 256 * 1024) file.delete();
            String line = new java.util.Date() + " " + message + System.lineSeparator();
            java.nio.file.Files.write(file.toPath(),
                    line.getBytes(java.nio.charset.StandardCharsets.UTF_8),
                    java.nio.file.StandardOpenOption.CREATE,
                    java.nio.file.StandardOpenOption.APPEND);
        } catch (Throwable t) {
            // Only a log.
        }
    }

    /**
     * The mutex every process holds while it rewrites the list: launchers, and the icon
     * handler's {@code apk-icon} runs.
     */
    private static String apkIconsLockName() {
        return "Local\\gesher-" + Integer.toHexString(
                Device.dataDir().toLowerCase(Locale.ROOT).hashCode()) + "-apk-icons";
    }

    /**
     * Drops the lines of {@code dropPackage} (but not the icons {@link #apkIcon} drew: they
     * are not its data directory's) or for {@code added}'s file, then adds it.
     */
    private static synchronized void rewriteApkIcons(String dropPackage, String added)
            throws java.io.IOException {
        long lock = Zygote.lock(apkIconsLockName(), 30_000);
        try {
            File list = new File(Device.dataDir(), APK_ICONS);
            File cache = new File(Device.dataDir(), APK_ICON_CACHE);
            String cachePrefix = Zygote.windowsPath(cache.getPath()).toLowerCase(Locale.ROOT)
                    + "\\";
            java.util.List<String> lines = new java.util.ArrayList<>();
            if (list.isFile()) {
                lines.addAll(java.nio.file.Files.readAllLines(list.toPath(),
                        java.nio.charset.StandardCharsets.UTF_8));
            }
            String addedKey = added != null ? key(added) : null;
            lines.removeIf(l -> l.isEmpty() || key(l).equals(addedKey)
                    || (dropPackage != null && l.endsWith("\t" + dropPackage)
                            && !icon(l).toLowerCase(Locale.ROOT).startsWith(cachePrefix)));
            if (added != null) lines.add(added);
            while (lines.size() > APK_ICONS_KEPT) lines.remove(0);
            list.getParentFile().mkdirs();
            // Explorer reads without this mutex. Publish a complete list so a reader
            // cannot mistake a partial write for a missing icon and cache the fallback.
            File tmp = File.createTempFile("apk-icons-", ".tmp", list.getParentFile());
            try {
                java.nio.file.Files.write(tmp.toPath(), lines,
                        java.nio.charset.StandardCharsets.UTF_8);
                java.nio.file.Files.move(tmp.toPath(), list.toPath(),
                        java.nio.file.StandardCopyOption.ATOMIC_MOVE,
                        java.nio.file.StandardCopyOption.REPLACE_EXISTING);
            } finally {
                tmp.delete();
            }
            // The drawn icons no line names any more (not one being drawn now: they are
            // written before their line).
            File[] drawn = cache.listFiles();
            if (drawn != null) {
                java.util.Set<String> named = new java.util.HashSet<>();
                for (String l : lines) named.add(icon(l).toLowerCase(Locale.ROOT));
                long now = System.currentTimeMillis();
                for (File f : drawn) {
                    if (!f.getName().endsWith(".ico")) continue;
                    if (named.contains(Zygote.windowsPath(f.getPath()).toLowerCase(Locale.ROOT))) {
                        continue;
                    }
                    if (now - f.lastModified() > APK_ICON_CACHE_GRACE_MS) f.delete();
                }
            }
        } finally {
            if (lock != 0) Zygote.unlock(lock);
        }
    }

    /** A line's file: its size and time. */
    private static String key(String line) {
        int first = line.indexOf('\t');
        int second = first < 0 ? -1 : line.indexOf('\t', first + 1);
        return second < 0 ? line : line.substring(0, second);
    }

    /** A line's .ico. */
    private static String icon(String line) {
        String k = key(line);
        if (k.length() >= line.length()) return "";
        int end = line.indexOf('\t', k.length() + 1);
        return end < 0 ? line.substring(k.length() + 1) : line.substring(k.length() + 1, end);
    }

    private static void removeShortcut(Properties shortcut) {
        String link = shortcut != null ? shortcut.getProperty("link") : null;
        if (link == null) return;
        if (Shell.exists(link)) {
            log(Shell.removeFile(link) ? "removed the Start menu shortcut " + link
                    : "cannot remove the Start menu shortcut " + link);
        }
        int slash = link.lastIndexOf('\\');
        if (slash > 0) {
            String parent = link.substring(0, slash);
            for (String language : UiMessages.supportedLanguages()) {
                if (parent.endsWith("\\" + UiMessages.text("apps_folder", language))) {
                    Shell.removeEmptyDirectory(parent);
                    break;
                }
            }
        }
    }

    /**
     * The shortcut's path: {@code <label>.lnk}, or {@code <label> (<package>).lnk} when another
     * app's shortcut has that name.
     */
    static String linkPath(String folder, String label, String packageName, String mine) {
        String name = fileName(label);
        String link = folder + "\\" + name + ".lnk";
        if (Shell.exists(link) && !link.equalsIgnoreCase(mine)) {
            link = folder + "\\" + name + " (" + packageName + ").lnk";
        }
        return link;
    }

    /** {@code label} as a Windows file name. */
    static String fileName(String label) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < label.length() && sb.length() < 80; i++) {
            char c = label.charAt(i);
            sb.append(c < 32 || "<>:\"/\\|?*".indexOf(c) >= 0 ? '_' : c);
        }
        String name = sb.toString().trim();
        while (name.endsWith(".") || name.endsWith(" ")) {
            name = name.substring(0, name.length() - 1);
        }
        if (name.isEmpty()) return text("android_app");
        String base = name.contains(".") ? name.substring(0, name.indexOf('.')) : name;
        if (base.toUpperCase(Locale.ROOT).matches("CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9]")) {
            name = name + "_";
        }
        return name;
    }

    /** Gesher's folder: dalvikvm.exe's. */
    private static String installDir() {
        String exe = Zygote.exePath();
        return exe.substring(0, exe.lastIndexOf('\\'));
    }

    private static ActivityInfo launcherActivity(ApkPackage pkg) {
        try {
            return pkg.launcherActivity(null);
        } catch (IllegalStateException e) {
            return null;
        }
    }

    // --- the tests' entry point -----------------------------------------------------------

    /**
     * {@code com.gesher.app.shell.AppShell icon APP.apk OUT.ico}: the label and the icon a
     * shortcut of the app gets, the icon written to OUT.ico (tests/windows/tracks/55-shell.ps1).
     * Exits 1 when no icon could be drawn.
     */
    public static void main(String[] args) {
        int code = 0;
        try {
            if (args.length != 3 || !args[0].equals("icon")) {
                System.err.println("usage: com.gesher.app.shell.AppShell icon APP.apk OUT.ico");
                code = 2;
            } else {
                String androidRoot = System.getenv("ANDROID_ROOT");
                if (androidRoot == null || androidRoot.isEmpty()) androidRoot = "/system";
                Main.bootstrap(TAG, androidRoot + "/build.prop",
                        androidRoot + "/framework/framework-res.apk");
                ApkPackage pkg = ApkPackage.parse(new File(args[1]).getAbsolutePath());
                AppIcon icon = AppIcon.load(pkg.apkPath, pkg.applicationInfo(0),
                        launcherActivity(pkg));
                log("label of " + pkg.packageName + ": " + icon.label);
                log("icon of " + pkg.packageName + ": " + icon.source);
                log("AppUserModelID of " + pkg.packageName + ": "
                        + appUserModelId(pkg.packageName));
                if (icon.hasImages()) {
                    icon.writeIco(new File(args[2]));
                    StringBuilder sizes = new StringBuilder();
                    for (int s : AppIcon.SIZES) {
                        sizes.append(sizes.length() > 0 ? "," : "").append(s);
                    }
                    log("wrote " + args[2] + ": " + AppIcon.SIZES.length + " sizes (" + sizes
                            + ")");
                } else {
                    code = 1;
                }
            }
        } catch (Throwable t) {
            System.out.flush();
            System.err.println(TAG + ": failed: " + t);
            t.printStackTrace();
            code = 1;
        }
        System.out.flush();
        System.err.flush();
        System.exit(code);
    }
}
