/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.ComponentName;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.pm.IShortcutService;
import android.content.pm.ParceledListSlice;
import android.content.pm.ShortcutInfo;
import android.os.PersistableBundle;
import java.util.List;
import java.util.Set;

/** Same public service calls in the host merge fixture and actual Windows persistence test. */
public final class ShortcutUpdateContractTest {
    private static int checks;
    private interface Work { void run() throws Exception; }
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    private static void rejects(Class<? extends Throwable> type, Work work, String message) throws Exception {
        try { work.run(); }
        catch (Throwable failure) {
            check(type.isInstance(failure), message + ": " + failure); return;
        }
        throw new AssertionError(message + ": no exception");
    }
    private static ShortcutInfo.Builder builder(String pkg, String id) {
        return new ShortcutInfo.Builder(new ContextWrapper(null) {
            @Override public String getPackageName() { return pkg; }
            @Override public int getUserId() { return 0; }
        }, id);
    }
    private static ParceledListSlice<ShortcutInfo> slice(ShortcutInfo... values) {
        return new ParceledListSlice<>(List.of(values));
    }
    @SuppressWarnings("unchecked")
    private static ShortcutInfo only(IShortcutService service, String pkg) throws Exception {
        List<ShortcutInfo> values = service.getShortcuts(pkg, 1, 0).getList();
        check(values.size() == 1, "update preserves one existing shortcut");
        return values.get(0);
    }
    public static int contracts(IShortcutService service, String pkg) throws Exception {
        checks = 0;
        String id = "partial-update";
        ComponentName activity = new ComponentName(pkg, pkg + ".ShortcutActivity");
        Intent originalIntent = new Intent(Intent.ACTION_VIEW).setComponent(activity);
        PersistableBundle originalExtras = new PersistableBundle();
        originalExtras.putString("before", "kept");
        ShortcutInfo original = builder(pkg, id).setShortLabel("original").setLongLabel("long label")
                .setActivity(activity).setIntent(originalIntent).setCategories(Set.of("original.category"))
                .setExtras(originalExtras).setRank(3).build();
        check(service.setDynamicShortcuts(pkg, slice(original), 0), "create initial shortcut");
        ShortcutInfo label = builder(pkg, id).setShortLabel("changed").build();
        check(service.updateShortcuts(pkg, slice(label), 0), "label-only update");
        ShortcutInfo value = only(service, pkg);
        check("changed".contentEquals(value.getShortLabel()), "supplied label changed");
        check("long label".contentEquals(value.getLongLabel()) && value.getRank() == 3,
                "omitted long label and rank retained");
        check(value.getActivity().equals(activity) && Intent.ACTION_VIEW.equals(value.getIntent().getAction()),
                "omitted activity and intent retained");
        check(value.getCategories().equals(Set.of("original.category"))
                && "kept".equals(value.getExtras().getString("before")), "omitted categories and extras retained");
        check(!label.hasRank() && label.getIntents() == null && label.getFlags() == 0,
                "partial input was not filled or flagged in place");

        check(service.updateShortcuts(pkg, slice(builder(pkg, id).setRank(0).build()), 0), "explicit zero-rank update");
        check(only(service, pkg).getRank() == 0, "explicit zero rank differs from omitted rank");
        PersistableBundle replacement = new PersistableBundle(); replacement.putInt("after", 7);
        check(service.updateShortcuts(pkg, slice(builder(pkg, id).setExtras(replacement).build()), 0), "extras-only update");
        value = only(service, pkg);
        check(value.getExtras().getInt("after") == 7 && !value.getExtras().containsKey("before"),
                "supplied extras replace previous extras");
        check("changed".contentEquals(value.getShortLabel()) && value.getRank() == 0, "extras update retains other fields");
        replacement.putInt("after", 99);
        check(only(service, pkg).getExtras().getInt("after") == 7, "publication detaches caller extras");

        Intent replacementIntent = new Intent(Intent.ACTION_EDIT).setComponent(activity)
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        check(service.updateShortcuts(pkg, slice(builder(pkg, id).setIntent(replacementIntent).build()), 0), "intent-only update");
        value = only(service, pkg);
        check(Intent.ACTION_EDIT.equals(value.getIntent().getAction())
                && pkg.equals(value.getIntent().getPackage()), "supplied intent retains publisher scope");
        check((value.getIntent().getFlags() & Intent.IMMUTABLE_FLAGS) == 0
                && replacementIntent.getFlags() != 0, "update sanitizes copied intent only");

        service.disableShortcuts(pkg, List.of(id), null, 0, 0);
        ShortcutInfo flagged = builder(pkg, id).setShortLabel("disabled update").build();
        flagged.addFlags(ShortcutInfo.FLAG_PINNED);
        check(service.updateShortcuts(pkg, slice(flagged), 0), "update a disabled shortcut");
        value = only(service, pkg);
        check(!value.isEnabled() && value.isDynamic() && !value.isPinned(), "update preserves stored flags");
        check(flagged.isPinned() && !flagged.isDynamic(), "input flags unchanged");
        check(service.updateShortcuts(pkg, slice(builder(pkg, "unknown").build()), 0), "unknown partial ID is a no-op");
        check(only(service, pkg).getId().equals(id), "unknown partial ID not created");
        check(service.updateShortcuts(pkg, slice(builder(pkg, id).build()), 0), "empty partial update accepted");
        check("disabled update".contentEquals(only(service, pkg).getShortLabel()), "empty update retains label");

        ShortcutInfo partial = builder(pkg, "new-id").setShortLabel("incomplete").build();
        rejects(IllegalArgumentException.class, () -> service.setDynamicShortcuts(pkg, slice(partial), 0), "creation still requires intent");
        rejects(IllegalArgumentException.class, () -> service.addDynamicShortcuts(pkg, slice(partial), 0), "add still requires intent");
        rejects(IllegalArgumentException.class, () -> service.pushDynamicShortcut(pkg, partial, 0), "push still requires intent");
        rejects(SecurityException.class, () -> service.updateShortcuts(pkg, slice(builder(pkg, id)
                .setActivity(new ComponentName("foreign.package", "Foreign")).build()), 0), "partial activity ownership");
        rejects(SecurityException.class, () -> service.updateShortcuts(pkg, slice(builder(pkg, id)
                .setIntent(new Intent(Intent.ACTION_VIEW).setPackage("foreign.package")).build()), 0), "partial intent ownership");
        rejects(SecurityException.class, () -> service.updateShortcuts(pkg, slice(builder("foreign.package", id).build()), 0),
                "partial publisher ownership");
        rejects(SecurityException.class, () -> service.updateShortcuts(pkg, slice(
                builder(pkg, id).setShortLabel("must not publish").build(),
                builder(pkg, id).setIntent(new Intent(Intent.ACTION_VIEW).setPackage("foreign.package")).build()), 0),
                "entire update batch validates before publication");
        check("disabled update".contentEquals(only(service, pkg).getShortLabel()), "rejected batch preserves stored state");

        check(service.updateShortcuts(pkg, slice(builder(pkg, id).setShortLabel("ordered").build(),
                builder(pkg, id).setRank(4).build()), 0), "successive partial updates of same ID");
        value = only(service, pkg);
        check("ordered".contentEquals(value.getShortLabel()) && value.getRank() == 4,
                "same-ID partial fields accumulate in order");
        service.enableShortcuts(pkg, List.of(id), 0);
        check(only(service, pkg).isEnabled(), "separate enable call still works");
        service.removeAllDynamicShortcuts(pkg, 0);
        System.out.println("ShortcutUpdateContractTest all passed: " + checks);
        return checks;
    }
}
