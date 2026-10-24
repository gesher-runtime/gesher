/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app.shell;

import android.content.pm.ActivityInfo;

import com.gesher.app.Main;
import com.gesher.server.ApkPackage;
import com.gesher.server.Device;
import com.gesher.server.Zygote;

import java.io.File;
import java.util.Locale;
import java.util.Properties;

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
 */
public final class AppShell {
    /** The Start menu folder of the apps, under the user's Programs folder. */
    public static final String FOLDER = "Gesher Apps";
    /**
     * The AppUserModelID an app process sets for itself (SetCurrentProcessExplicitAppUserModelID,
     * {@link #applyProcessAppUserModelId}): the system server puts it in the environment of the
     * processes it starts for a package.
     */
    public static final String ENV_APP_USER_MODEL_ID = "GESHER_APP_USER_MODEL_ID";
    /** {@code GESHER_SHORTCUTS=0}: no Start menu shortcuts. */
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
        String target = gesher + "\\gesher-app.cmd";
        if (!Shell.exists(target)) {
            log("no Start menu shortcut for " + packageName + ": no " + target
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
        if (!changed && oldLink != null && codePath.equals(old.getProperty("code"))
                && Shell.exists(oldLink)) {
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
        int hr = Shell.createShortcut(link, target, "launch " + packageName, gesher, iconPath,
                icon.label + " (" + packageName + "), an Android app on Gesher", id);
        if (hr != 0) {
            throw new IllegalStateException(String.format("saving %s failed: HRESULT 0x%08x",
                    link, hr));
        }
        if (oldLink != null && !oldLink.equalsIgnoreCase(link)) Shell.removeFile(oldLink);
        Properties record = new Properties();
        record.setProperty("package", packageName);
        record.setProperty("link", link);
        record.setProperty("label", icon.label);
        record.setProperty("icon", iconPath);
        record.setProperty("code", codePath);
        record.setProperty("app_user_model_id", id);
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
        } catch (Throwable t) {
            log("the Start menu shortcut of " + packageName + " was not removed: " + t);
        }
    }

    private static void removeShortcut(Properties shortcut) {
        String link = shortcut != null ? shortcut.getProperty("link") : null;
        if (link == null) return;
        if (Shell.exists(link)) {
            log(Shell.removeFile(link) ? "removed the Start menu shortcut " + link
                    : "cannot remove the Start menu shortcut " + link);
        }
        int slash = link.lastIndexOf('\\');
        if (slash > 0 && link.substring(0, slash).endsWith("\\" + FOLDER)) {
            Shell.removeEmptyDirectory(link.substring(0, slash));
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
        if (name.isEmpty()) return "Android app";
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
