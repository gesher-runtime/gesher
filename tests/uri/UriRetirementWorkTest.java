/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package gesher.services;

import android.content.ClipData;
import android.content.Intent;
import android.net.Uri;
import android.os.IBinder;
import com.gesher.server.UriGrants;
import java.lang.reflect.Field;
import static gesher.services.UriGrantRetirementContractTest.*;

/** Counters are injected into a separate complete-source build, never timed code. */
public final class UriRetirementWorkTest {
    private static Field visits;

    private static long count() throws Exception { return visits.getLong(null); }
    private static void reset() throws Exception { visits.setLong(null, 0); }

    private static void budget(String name, long expected) throws Exception {
        long actual = count();
        check(actual == expected, "Actual retirement work changed for " + name + ": " + actual + " != " + expected);
        System.out.println("URI_RETIREMENT_WORK case=" + name + " visits=" + actual + " expected=" + expected);
    }

    public static void run(boolean baseline) throws Exception {
        initialize(false);
        visits = UriGrants.class.getDeclaredField("retirementVisits");
        visits.setAccessible(true);
        try (Fixture f = new Fixture()) {
            f.seed(CLIENT, 128);
            f.as(CLIENT, () -> check(f.explicit(CLIENT, uri(0), READ), "Warm query missing"));
            reset();
            f.as(CLIENT, () -> {
                for (int i = 0; i < 128; i++) check(f.explicit(CLIENT, uri(i), READ), "Counted exact query lost permission");
            });
            budget("exact128", baseline ? 128L * 128 : 0);
            f.replace(OWNER);
            reset();
            f.as(CLIENT, () -> check(!f.explicit(CLIENT, uri(0), READ), "Counted replacement retained permission"));
            budget("retired128", 128);
            check(f.grantCount(CLIENT) == 0, "Counted retirement left table ownership");
        }
        try (Fixture f = new Fixture()) {
            IBinder owner = f.owner();
            Intent intent = new Intent().setData(uri(0)).addFlags(READ);
            ClipData clip = ClipData.newRawUri("complete owned grant batch", uri(1));
            for (int i = 2; i < 128; i++) clip.addItem(new ClipData.Item(uri(i)));
            intent.setClipData(clip);
            reset();
            f.as(OWNER, () -> f.owned(owner, CLIENT, intent));
            budget("owned_batch128", baseline ? 128L * 127 / 2 : 0);
            f.as(CLIENT, () -> {
                for (int i = 0; i < 128; i++) check(f.explicit(CLIENT, uri(i), READ), "Counted owned batch lost a URI");
            });
            f.retire(owner);
            check(f.grantCount(CLIENT) == 0, "Counted batch retained its owner");
        }
        try (Fixture f = new Fixture()) {
            f.seed(CLIENT, 128);
            Object registration = f.hosted("fixture.work.hosted");
            Uri hosted = Uri.parse("content://fixture.work.hosted/item");
            f.hostedGrant(registration, CLIENT, hosted);
            reset();
            f.as(CLIENT, () -> {
                for (int i = 0; i < 128; i++) check(f.explicit(CLIENT, uri(i), READ), "Hosted fallback lost installed permission");
            });
            budget("mixed_hosted128", 129L * 128);
            f.as(CLIENT, () -> check(f.explicit(CLIENT, hosted, READ), "Counted hosted grant missing"));
        }
        System.out.println("URI_RETIREMENT_WORK_COMPLETE variant=" + (baseline ? "baseline" : "candidate")
                + " complete_source=true counters_not_timing=true");
    }
}
