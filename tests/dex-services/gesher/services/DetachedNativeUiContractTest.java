/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import static gesher.services.SafDocumentsContractTest.*;

import android.app.ActivityManager;
import android.app.servertransaction.ResumeActivityItem;
import android.content.Intent;
import java.io.File;
import java.nio.file.Files;
import java.util.Comparator;
import java.util.List;
import java.util.concurrent.TimeUnit;

/** Detached native windows still cover the focused task beneath another native window. */
public final class DetachedNativeUiContractTest {
    private static int checks;
    private static void check(boolean condition, String reason) {
        if (!condition) throw new AssertionError(reason);
        checks++;
    }

    private static void overlap(File dir, String first) throws Exception {
        Harness h = new Harness(new File(dir, first + ".xml"));
        Object source = h.activity("Origin", "RESUMED");
        Dialog documents = new Dialog(); documents.install(h);
        ChoiceDialog choices = new ChoiceDialog(); choices.install(h);
        try {
            Intent documentRequest = open(), choiceRequest = chooser();
            if (first.equals("document")) {
                choiceRequest.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                check(h.start(documentRequest, source, 41) == ActivityManager.START_SUCCESS,
                        "First document picker starts");
                check(documents.opened.await(3, TimeUnit.SECONDS), "First document picker opens");
                check(h.start(choiceRequest, null, -1) == ActivityManager.START_SUCCESS,
                        "Detached chooser starts over document picker");
            } else {
                documentRequest.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                check(h.start(choiceRequest, source, 42) == ActivityManager.START_SUCCESS,
                        "First chooser starts");
                check(choices.opened.await(3, TimeUnit.SECONDS), "First chooser opens");
                check(h.start(documentRequest, null, -1) == ActivityManager.START_SUCCESS,
                        "Detached document picker starts over chooser");
            }
            check(documents.opened.await(3, TimeUnit.SECONDS) && choices.opened.await(3, TimeUnit.SECONDS),
                    "Both real native workers enter their declared UI boundaries");
            if (first.equals("document")) {
                documents.finish.countDown(); awaitPending(h, 0, 1); workersRetired("document-picker");
            } else {
                choices.finish.countDown(); awaitPending(h, 1, 0); workersRetired("intent-chooser");
            }
            check(get(source, "state") == state("PAUSED") && h.thread.results().isEmpty()
                    && h.thread.events.stream().noneMatch(item -> item instanceof ResumeActivityItem),
                    "Closing " + first + " must not resume the source beneath the detached second native window");
            check(h.idleSince() == 0, "Detached second window retains actual busy ownership");
            documents.finish.countDown(); choices.finish.countDown();
            h.settled(); workersRetired("intent-chooser"); awaitPending(h, 0, 0);
            check(get(source, "state") == state("RESUMED") && h.thread.results().size() == 1
                    && h.thread.events.stream().filter(item -> item instanceof ResumeActivityItem).count() == 1,
                    "Final native close resumes once with only the attached caller's result");
            System.out.println("DetachedNativeUiContractTest " + first + "-first passed");
        } finally {
            documents.finish.countDown(); choices.finish.countDown();
            h.settled(); workersRetired("intent-chooser"); h.grants.flush();
        }
    }

    public static void contracts(String order) throws Exception {
        field(Class.forName("com.gesher.app.locale.SystemLocale"), "sIs24Hour").set(null, Boolean.TRUE);
        File dir = Files.createTempDirectory("gesher-native-ui-detached-").toFile();
        try {
            for (String first : order == null ? List.of("document", "chooser") : List.of(order)) overlap(dir, first);
            System.out.println("DetachedNativeUiContractTest passed: " + checks + " checks");
        } finally {
            try (var paths = Files.walk(dir.toPath())) {
                for (var path : paths.sorted(Comparator.reverseOrder()).toList()) Files.deleteIfExists(path);
            }
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length > 0) {
            System.load(args[0]);
            android.app.PropertyInvalidatedCache.setTestMode(true);
            android.app.PropertyInvalidatedCache.disableForTestMode();
        } else System.loadLibrary("android_runtime");
        contracts(args.length > 1 ? args[1] : null);
    }
}
