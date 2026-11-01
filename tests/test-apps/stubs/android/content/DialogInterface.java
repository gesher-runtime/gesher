/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package android.content;

public interface DialogInterface {
    void cancel();
    void dismiss();

    interface OnClickListener {
        void onClick(DialogInterface dialog, int which);
    }

    interface OnCancelListener {
        void onCancel(DialogInterface dialog);
    }
}
