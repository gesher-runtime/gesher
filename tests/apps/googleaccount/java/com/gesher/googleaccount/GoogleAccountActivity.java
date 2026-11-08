/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
package com.gesher.googleaccount;

import android.accounts.Account;
import android.accounts.AccountManager;
import android.accounts.AuthenticatorDescription;
import android.app.Activity;
import android.app.AlertDialog;
import android.os.Bundle;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

import javax.net.ssl.HttpsURLConnection;

/** A manual, separate app: uses public AccountManager APIs, never reads owner credentials. */
public final class GoogleAccountActivity extends Activity {
    private static final String TYPE = "com.google";
    private static final String SCOPE = "oauth2:https://www.googleapis.com/auth/userinfo.email";
    private final ExecutorService worker = Executors.newSingleThreadExecutor(task -> {
        Thread thread = new Thread(task, "Google account verification");
        thread.setDaemon(true);
        return thread;
    });
    private AccountManager accounts;
    private TextView status;
    private Button connect, verify, refresh;
    private boolean busy;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        accounts = AccountManager.get(this);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (20 * getResources().getDisplayMetrics().density);
        content.setPadding(pad, pad, pad, pad);
        TextView title = new TextView(this);
        title.setText("בדיקת חשבון Google מתוך אפליקציה");
        title.setTextSize(23);
        content.addView(title);
        TextView explanation = new TextView(this);
        explanation.setText("1. התחבר דרך האפליקציה כדי לאפשר לה לראות את החשבון.\n"
                + "2. לחץ על בדיקת הגישה ואשר בעצמך בקשת הרשאה אם תופיע.\n"
                + "הבדיקה מבקשת גישה בסיסית לפרטי החשבון ופונה לשרת Google לאימות. "
                + "היא אינה שומרת או מציגה סיסמאות ואסימוני גישה.");
        explanation.setTextSize(17);
        content.addView(explanation);
        connect = button(content, "1. התחברות דרך אפליקציית הבדיקה", this::connect);
        verify = button(content, "2. בדיקת גישה אמיתית ל־Google", this::chooseAndVerify);
        refresh = button(content, "רענון מצב החשבון", this::refresh);
        status = new TextView(this);
        status.setTextSize(18);
        content.addView(status);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(content);
        setContentView(scroll);
        refresh();
    }

    private Button button(LinearLayout content, String label, Runnable action) {
        Button button = new Button(this);
        button.setText(label);
        button.setOnClickListener(view -> { if (!busy) action.run(); });
        content.addView(button);
        return button;
    }

    private void busy(boolean value) {
        busy = value;
        connect.setEnabled(!value);
        verify.setEnabled(!value);
        refresh.setEnabled(!value);
    }

    private void refresh() {
        try {
            boolean google = false;
            for (AuthenticatorDescription authenticator : accounts.getAuthenticatorTypes()) {
                if (TYPE.equals(authenticator.type)
                        && "com.google.android.gms".equals(authenticator.packageName)) google = true;
            }
            int count = accounts.getAccountsByType(TYPE).length;
            status.setText("שירות חשבון Google: " + (google ? "מזוהה" : "לא מזוהה")
                    + "\nחשבונות הזמינים לאפליקציה הזו: " + count
                    + "\n\nרק אישור מוצלח משרת Google בשלב 2 מוכיח שהגישה עובדת.");
        } catch (RuntimeException error) { failed("קריאת מצב החשבון", error); }
    }

    private void connect() {
        busy(true);
        status.setText("ממתין להשלמת ההתחברות שלך…");
        try {
            accounts.addAccount(TYPE, null, null, null, this, future -> {
                try {
                    Bundle result = future.getResult();
                    if (!TYPE.equals(result.getString(AccountManager.KEY_ACCOUNT_TYPE))
                            || result.getString(AccountManager.KEY_ACCOUNT_NAME) == null) {
                        status.setText("ההתחברות הסתיימה בלי חשבון Google מאומת.");
                    } else {
                        refresh();
                    }
                } catch (Exception error) { failed("השלמת ההתחברות", error); }
                finally { busy(false); }
            }, null);
        } catch (RuntimeException error) { busy(false); failed("פתיחת ההתחברות", error); }
    }

    private void chooseAndVerify() {
        try {
            Account[] visible = accounts.getAccountsByType(TYPE);
            if (visible.length == 0) {
                status.setText("אין חשבון זמין לאפליקציה. השלם תחילה את שלב 1.");
            } else if (visible.length == 1) {
                requestToken(visible[0]);
            } else {
                String[] labels = new String[visible.length];
                for (int i = 0; i < visible.length; i++) labels[i] = visible[i].name;
                new AlertDialog.Builder(this).setTitle("בחר חשבון לבדיקה")
                        .setItems(labels, (dialog, index) -> requestToken(visible[index]))
                        .setNegativeButton("ביטול", null).show();
            }
        } catch (RuntimeException error) { failed("בחירת חשבון", error); }
    }

    private void requestToken(Account account) {
        busy(true);
        status.setText("מבקש מ־microG גישה לחשבון עבור האפליקציה הזו…");
        try {
            accounts.getAuthToken(account, SCOPE, null, this, future -> {
                try {
                    Bundle result = future.getResult();
                    String token = result.getString(AccountManager.KEY_AUTHTOKEN);
                    if (token == null || token.isEmpty() || token.startsWith("dummy_token_gesher_")) {
                        status.setText("לא התקבל אסימון גישה אמיתי. הבדיקה לא עברה.");
                        busy(false);
                        return;
                    }
                    status.setText("התקבל אסימון. ממתין לאישור משרת Google…");
                    worker.execute(() -> verifyWithGoogle(account, token));
                } catch (Exception error) { busy(false); failed("קבלת גישה לחשבון", error); }
            }, null);
        } catch (RuntimeException error) { busy(false); failed("בקשת גישה לחשבון", error); }
    }

    private void verifyWithGoogle(Account account, String token) {
        HttpsURLConnection connection = null;
        String message;
        try {
            connection = (HttpsURLConnection) new URL(
                    "https://www.googleapis.com/oauth2/v2/userinfo").openConnection();
            connection.setInstanceFollowRedirects(false);
            connection.setUseCaches(false);
            connection.setConnectTimeout(15000);
            connection.setReadTimeout(15000);
            connection.setRequestProperty("Authorization", "Bearer " + token);
            connection.setRequestProperty("Accept", "application/json");
            int code = connection.getResponseCode();
            if (code == 200) {
                ByteArrayOutputStream bytes = new ByteArrayOutputStream();
                try (InputStream input = connection.getInputStream()) {
                    byte[] chunk = new byte[4096];
                    for (int count; (count = input.read(chunk)) >= 0;) {
                        if (bytes.size() + count > 65536) throw new IllegalStateException();
                        bytes.write(chunk, 0, count);
                    }
                }
                JSONObject result = new JSONObject(bytes.toString(StandardCharsets.UTF_8.name()));
                Object id = result.opt("id"), email = result.opt("email");
                message = id instanceof String && !((String) id).isEmpty()
                        && email instanceof String && account.name.equalsIgnoreCase((String) email)
                        ? "הבדיקה עברה: Google אישרה את אסימון הגישה שהאפליקציה קיבלה מ־microG.\n"
                                + "כעת אפשר לסגור ולפתוח את Gesher ולחזור על שלב 2 לבדיקת שמירת החשבון."
                        : "תשובת Google אינה תואמת לחשבון שנבחר. הבדיקה לא עברה.";
            } else {
                message = "Google לא אישרה את הגישה (HTTP " + code + "). הבדיקה לא עברה.";
            }
        } catch (Exception error) {
            message = "אימות הגישה לא הושלם: " + error.getClass().getSimpleName()
                    + ". הבדיקה לא עברה.";
        } finally { if (connection != null) connection.disconnect(); }
        final String outcome = message;
        runOnUiThread(() -> { if (!isFinishing()) { status.setText(outcome); busy(false); } });
    }

    private void failed(String step, Exception error) {
        // Exception messages/bundles can contain personal data. Never log or render them.
        status.setText(step + " לא הושלם: " + error.getClass().getSimpleName());
    }

    @Override protected void onDestroy() { worker.shutdownNow(); super.onDestroy(); }
}
