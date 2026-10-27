package com.gesher.sharedapp;

import android.app.Activity;
import android.os.Bundle;

import com.gesher.shared.base.Base;
import com.gesher.shared.entry.Entry;
import com.gesher.shared.order.Marker;

public final class SharedActivity extends Activity {
    public static int hot(int count) {
        int answer = 0;
        for (int i = 0; i < count; ++i) answer += Entry.value();
        return answer;
    }

    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        ClassLoader app = getClassLoader();
        ClassLoader entry = Entry.class.getClassLoader();
        ClassLoader dependency = Base.class.getClassLoader();
        if (entry == app || dependency == app || entry == dependency) {
            throw new AssertionError("shared Java jars were flattened into one app/boot loader");
        }
        if (!Marker.value().equals("library-before")) {
            throw new AssertionError("the actual shared loader did not precede the APK duplicate");
        }
        System.out.println("SharedActivity result=" + hot(1000)
                + "; dependency=" + Base.value() + "; order=" + Marker.value()
                + "; distinct app/entry/dependency loaders=true");
    }
}
