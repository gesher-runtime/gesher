/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.app;

import android.content.Context;
import android.content.DialogInterface;

public class AlertDialog extends Dialog {
    public AlertDialog(Context context) { super(context); }

    public static class Builder {
        public Builder(Context context) {}
        public Builder setTitle(CharSequence title) { return this; }
        public Builder setMessage(CharSequence message) { return this; }
        public Builder setPositiveButton(int textId, DialogInterface.OnClickListener listener) { return this; }
        public Builder setNegativeButton(int textId, DialogInterface.OnClickListener listener) { return this; }
        public Builder setOnCancelListener(DialogInterface.OnCancelListener listener) { return this; }
        public AlertDialog create() { return new AlertDialog(null); }
        public AlertDialog show() { return create(); }
    }
}
