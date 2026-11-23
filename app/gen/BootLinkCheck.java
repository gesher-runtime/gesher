/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */

import java.io.ByteArrayInputStream;
import java.io.DataInputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;
import java.util.stream.Stream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * Checks that every member framework.jar references on a class gesher-app.jar supplies
 * exists on gesher-app.jar's class (or one of its supertypes).
 *
 * <p>On a device framework.jar and the mainline module jars (framework-bluetooth,
 * framework-wifi...) are built together, so framework code may call module classes'
 * hidden members: {@code Intent.prepareToEnterProcess} calls
 * {@code BluetoothDevice.prepareToEnterProcess(AttributionSource)} on a Bluetooth
 * broadcast's EXTRA_DEVICE. Where gesher-app.jar supplies such a class itself, a member it
 * lacks is a NoSuchMethodError or NoSuchFieldError only when the framework path runs
 * (issue #303). This reads the constant pools of framework.jar's classes, keeps the
 * method and field references that framework classes gesher-app.jar does not replace
 * make to classes gesher-app.jar defines, and resolves each one against gesher-app.jar's
 * class hierarchy, falling back to framework.jar and libcore for supertypes.
 * FRAMEWORK is the compile classes (tools/framework/build.py), which include the module
 * API classes framework.jar compiles against but does not ship; gesher-app.jar's copies of
 * those are the ones that run.
 *
 * <pre>java BootLinkCheck.java FRAMEWORK GESHER_APP_CLASSES [CORE] [--allow FILE]</pre>
 *
 * FRAMEWORK and GESHER_APP_CLASSES are jars or class directories; CORE is libcore's
 * java.base (an exploded module). Unresolved references not listed in the allow file
 * (lines {@code owner.name:descriptor}, '#' comments) are printed with the framework
 * classes that use them, and the exit status is 1. An allow-file entry that is no longer
 * unresolved is reported as stale (a warning: remove it).
 */
public final class BootLinkCheck {
    /** A class file's declared members and supertypes. */
    record ClassInfo(String name, String superName, List<String> interfaces,
            Set<String> methods, Set<String> fields) {
    }

    /** A member reference: kind 'M' (method) or 'F' (field). */
    record Ref(char kind, String owner, String name, String desc) {
        String key() {
            return owner.replace('/', '.') + "." + name + ":" + desc;
        }
    }

    private final Map<String, ClassInfo> mGesher = new HashMap<>();
    private final Map<String, ClassInfo> mOther = new HashMap<>();

    public static void main(String[] args) throws IOException {
        List<String> positional = new ArrayList<>();
        Path allowFile = null;
        for (int i = 0; i < args.length; i++) {
            if (args[i].equals("--allow")) {
                allowFile = Path.of(args[++i]);
            } else {
                positional.add(args[i]);
            }
        }
        if (positional.size() < 2) {
            System.err.println("usage: BootLinkCheck FRAMEWORK GESHER_APP_CLASSES [CORE] [--allow FILE]");
            System.exit(2);
        }
        Set<String> allowed = new HashSet<>();
        if (allowFile != null) {
            for (String line : Files.readAllLines(allowFile)) {
                line = line.strip();
                if (!line.isEmpty() && !line.startsWith("#")) allowed.add(line);
            }
        }
        BootLinkCheck check = new BootLinkCheck();
        Map<String, byte[]> framework = read(Path.of(positional.get(0)));
        Map<String, byte[]> gesher = read(Path.of(positional.get(1)));
        for (Map.Entry<String, byte[]> e : gesher.entrySet()) {
            check.mGesher.put(e.getKey(), parse(e.getValue(), null));
        }
        if (positional.size() > 2) {
            for (byte[] b : read(Path.of(positional.get(2))).values()) {
                ClassInfo c = parse(b, null);
                check.mOther.put(c.name, c);
            }
        }
        // Framework references, by owner, for owners only gesher-app.jar supplies at run time.
        Map<String, Set<String>> users = new TreeMap<>();
        Map<String, Ref> refs = new HashMap<>();
        for (byte[] b : framework.values()) {
            List<Ref> found = new ArrayList<>();
            ClassInfo c = parse(b, found);
            check.mOther.put(c.name, c);
            // A framework class gesher-app.jar also defines runs as Gesher's copy.
            if (check.mGesher.containsKey(c.name)) continue;
            for (Ref r : found) {
                refs.putIfAbsent(r.key(), r);
                users.computeIfAbsent(r.key(), k -> new TreeSet<>()).add(c.name.replace('/', '.'));
            }
        }
        int missing = 0, checked = 0;
        Set<String> used = new HashSet<>();
        for (Map.Entry<String, Set<String>> e : users.entrySet()) {
            Ref r = refs.get(e.getKey());
            if (!check.mGesher.containsKey(r.owner)) continue;
            checked++;
            if (check.resolves(r, r.owner, new HashSet<>())) continue;
            if (allowed.contains(r.key())) {
                used.add(r.key());
                continue;
            }
            missing++;
            System.out.println("missing " + (r.kind == 'M' ? "method " : "field ") + r.key()
                    + "  (used by " + String.join(", ", e.getValue()) + ")");
        }
        for (String stale : new TreeSet<>(allowed)) {
            if (!used.contains(stale)) {
                System.out.println("warning: stale allow entry (resolves or is unused now): " + stale);
            }
        }
        System.out.println("boot link check: " + checked + " framework references to gesher-app classes, "
                + missing + " unresolved");
        System.exit(missing == 0 ? 0 : 1);
    }

    private boolean resolves(Ref r, String owner, Set<String> seen) {
        if (owner == null || !seen.add(owner)) return false;
        ClassInfo c = mGesher.get(owner);
        if (c == null) c = mOther.get(owner);
        if (c == null) {
            // A supertype outside every input (java.base without CORE): assume it declares
            // the member rather than report what cannot be checked.
            return !owner.startsWith("android/") && !owner.startsWith("com/android/");
        }
        Set<String> members = r.kind == 'M' ? c.methods : c.fields;
        if (r.kind == 'M' ? members.contains(r.name + r.desc) : members.contains(r.name + ":" + r.desc)) {
            return true;
        }
        if (resolves(r, c.superName, seen)) return true;
        for (String i : c.interfaces) {
            if (resolves(r, i, seen)) return true;
        }
        return false;
    }

    /** The classes of a jar or directory, by internal name. */
    private static Map<String, byte[]> read(Path p) throws IOException {
        Map<String, byte[]> out = new HashMap<>();
        if (Files.isDirectory(p)) {
            try (Stream<Path> s = Files.walk(p)) {
                for (Path f : (Iterable<Path>) s::iterator) {
                    String rel = p.relativize(f).toString().replace(File.separatorChar, '/');
                    if (rel.endsWith(".class") && !rel.endsWith("module-info.class")) {
                        out.put(rel.substring(0, rel.length() - 6), Files.readAllBytes(f));
                    }
                }
            }
        } else {
            try (ZipFile z = new ZipFile(p.toFile())) {
                for (ZipEntry e : java.util.Collections.list(z.entries())) {
                    String n = e.getName();
                    if (n.endsWith(".class") && !n.endsWith("module-info.class")) {
                        try (InputStream in = z.getInputStream(e)) {
                            out.put(n.substring(0, n.length() - 6), in.readAllBytes());
                        }
                    }
                }
            }
        }
        return out;
    }

    /** Parses a class file; adds its method and field references to {@code refs} if given. */
    private static ClassInfo parse(byte[] bytes, List<Ref> refs) throws IOException {
        DataInputStream in = new DataInputStream(new ByteArrayInputStream(bytes));
        if (in.readInt() != 0xCAFEBABE) throw new IOException("not a class file");
        in.readUnsignedShort();
        in.readUnsignedShort();
        int count = in.readUnsignedShort();
        int[] tag = new int[count];
        String[] utf = new String[count];
        int[] a = new int[count];
        int[] b = new int[count];
        for (int i = 1; i < count; i++) {
            tag[i] = in.readUnsignedByte();
            switch (tag[i]) {
                case 1 -> utf[i] = in.readUTF();
                case 3, 4 -> in.readInt();
                case 5, 6 -> { in.readLong(); i++; }
                case 7, 8, 16, 19, 20 -> a[i] = in.readUnsignedShort();
                case 9, 10, 11, 12, 17, 18 -> { a[i] = in.readUnsignedShort(); b[i] = in.readUnsignedShort(); }
                case 15 -> { in.readUnsignedByte(); in.readUnsignedShort(); }
                default -> throw new IOException("constant tag " + tag[i]);
            }
        }
        if (refs != null) {
            for (int i = 1; i < count; i++) {
                if (tag[i] == 9 || tag[i] == 10 || tag[i] == 11) {
                    String owner = utf[a[a[i]]];
                    if (owner.startsWith("[")) continue;  // array clone()
                    int nat = b[i];
                    refs.add(new Ref(tag[i] == 9 ? 'F' : 'M', owner, utf[a[nat]], utf[b[nat]]));
                }
            }
        }
        in.readUnsignedShort();
        String name = utf[a[in.readUnsignedShort()]];
        int superIndex = in.readUnsignedShort();
        String superName = superIndex == 0 ? null : utf[a[superIndex]];
        List<String> interfaces = new ArrayList<>();
        for (int n = in.readUnsignedShort(); n > 0; n--) interfaces.add(utf[a[in.readUnsignedShort()]]);
        Set<String> fields = new HashSet<>();
        for (int n = in.readUnsignedShort(); n > 0; n--) {
            in.readUnsignedShort();
            fields.add(utf[in.readUnsignedShort()] + ":" + utf[in.readUnsignedShort()]);
            skipAttributes(in);
        }
        Set<String> methods = new HashSet<>();
        for (int n = in.readUnsignedShort(); n > 0; n--) {
            in.readUnsignedShort();
            methods.add(utf[in.readUnsignedShort()] + utf[in.readUnsignedShort()]);
            skipAttributes(in);
        }
        return new ClassInfo(name, superName, interfaces, methods, fields);
    }

    private static void skipAttributes(DataInputStream in) throws IOException {
        for (int n = in.readUnsignedShort(); n > 0; n--) {
            in.readUnsignedShort();
            in.skipNBytes(in.readInt() & 0xffffffffL);
        }
    }
}
