package gesher;

import android.content.pm.ApplicationInfo;
import android.content.pm.SharedLibraryInfo;

import com.gesher.app.Main;
import com.gesher.server.SharedLibraryGraph;
import com.gesher.server.SharedLibraryRegistry;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.List;

/** Real XML and DEX-jar graph validation under the shipped ART/framework. */
public final class Aot3SharedLibraryTest {
    private static int checks;

    private static void check(boolean value, String name) {
        if (!value) throw new AssertionError(name);
        ++checks;
        System.out.println("PASS aot3 shared graph: " + name);
    }

    private static String attribute(String value) {
        return value.replace("&", "&amp;").replace("\"", "&quot;").replace("<", "&lt;");
    }

    private static String library(String name, File jar, String attributes) {
        return "<library name=\"" + name + "\" file=\"" + attribute(jar.getPath()) + "\" "
                + attributes + "/>";
    }

    private static SharedLibraryRegistry registry(File root, String xml) throws Exception {
        File file = new File(root, "etc/permissions/shared.xml");
        file.getParentFile().mkdirs();
        Files.write(file.toPath(), ("<permissions>" + xml + "</permissions>").getBytes(StandardCharsets.UTF_8));
        return SharedLibraryRegistry.read(root);
    }

    private static void rejects(SharedLibraryRegistry registry, String reason) throws Exception {
        try {
            registry.resolve(List.of("entry"), List.of(), null);
            throw new AssertionError("accepted invalid graph: " + reason);
        } catch (IOException expected) {
            check(expected.getMessage().contains(reason), "reject " + reason);
        }
    }

    public static void main(String[] args) throws Exception {
        Main.bootstrap("shared graph", args[0], args[1]);
        File root = new File(args[2]);
        File original = new File(args[3]);
        File updated = new File(args[4]);
        File entry = new File(args[5]);
        File base = new File(root, "real jars אב α/base.jar");
        base.getParentFile().mkdirs();
        Files.copy(original.toPath(), base.toPath(), StandardCopyOption.REPLACE_EXISTING);
        String ordinary = library("base", base, "") + library("entry", entry, "dependency=\"base\"");
        SharedLibraryRegistry normal = registry(root, ordinary);
        SharedLibraryGraph first = normal.resolve(List.of("entry"), List.of("absent"), null);
        ApplicationInfo app = new ApplicationInfo();
        first.apply(app);
        SharedLibraryInfo top = app.sharedLibraryInfos.get(0);
        SharedLibraryInfo dependency = top.getDependencies().get(0);
        check(app.sharedLibraryInfos.size() == 1 && top.getName().equals("entry")
                        && dependency.getName().equals("base") && app.sharedLibraryFiles.length == 2,
                "real SharedLibraryInfo preserves the dependency graph and two jar paths");
        check(first.classLoaderContext().equals("PCL[]{PCL[" + top.getPath() + "]{PCL["
                        + dependency.getPath() + "]}}"),
                "ART context describes those same canonical real jar nodes");
        check(first.contentsUnchanged(), "the snapshot hashes actual readable DEX jars");
        File oat = new File(root, "oat");
        oat.mkdirs();
        first.writeManifest(oat);
        check(first.matchesArtifacts(oat), "the published graph manifest matches its current contents/context");
        Files.copy(updated.toPath(), base.toPath(), StandardCopyOption.REPLACE_EXISTING);
        SharedLibraryGraph second = normal.resolve(List.of("entry"), List.of(), null);
        check(!first.contentsUnchanged() && first.classLoaderContext().equals(second.classLoaderContext())
                        && !first.identity().equals(second.identity()) && !second.matchesArtifacts(oat),
                "same-path dependency content update invalidates the old artifact identity");
        check(normal.resolve(List.of(), List.of("absent"), null).isEmpty(), "absent optional library contributes no loader");
        rejects(registry(root, ""), "INSTALL_FAILED_MISSING_SHARED_LIBRARY");
        rejects(registry(root, library("entry", entry, "dependency=\"missing\"")), "missing shared-library dependency");
        rejects(registry(root, library("entry", entry, "dependency=\"base\"")
                + library("base", base, "dependency=\"entry\"")), "cyclic shared-library dependency");
        rejects(registry(root, library("entry", entry, "min-device-sdk=\"999\"")), "INSTALL_FAILED_MISSING_SHARED_LIBRARY");
        rejects(registry(root, library("entry", entry, "max-device-sdk=\"1\"")), "INSTALL_FAILED_MISSING_SHARED_LIBRARY");
        rejects(registry(root, library("entry", entry, "min-device-sdk=\"R\"")), "INSTALL_FAILED_MISSING_SHARED_LIBRARY");
        check(!registry(root, library("entry", entry, "max-device-sdk=\"R\""))
                        .resolve(List.of("entry"), List.of(), null).isEmpty(),
                "known finalized codename uses the pinned SystemConfig maximum-SDK wrapper");
        rejects(registry(root, library("entry", entry, "on-bootclasspath-since=\"1\"")),
                "declared on boot class path but absent");
        check(normal.resolve(List.of("entry"), List.of(), "dalvik.system.DelegateLastClassLoader")
                        .classLoaderContext().startsWith("DLC[]{PCL["),
                "declared DelegateLastClassLoader changes the actual app context descriptor");
        System.out.println("Aot3SharedLibraryTest all passed: " + checks + " real-jar/registry controls");
    }
}
