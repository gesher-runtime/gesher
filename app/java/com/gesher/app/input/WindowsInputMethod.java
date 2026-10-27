/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.app.input;

import android.graphics.Matrix;
import android.graphics.RectF;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Looper;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.SystemClock;
import android.text.SpannableString;
import android.text.Spanned;
import android.text.style.BackgroundColorSpan;
import android.text.style.UnderlineSpan;
import android.util.ArrayMap;
import android.view.InputChannel;
import android.view.InputEvent;
import android.view.InputEventReceiver;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewRootImpl;
import android.view.WindowInsets;
import android.view.inputmethod.CompletionInfo;
import android.view.inputmethod.CursorAnchorInfo;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.ExtractedText;
import android.view.inputmethod.ExtractedTextRequest;
import android.view.inputmethod.InputConnection;

import com.android.internal.infra.AndroidFuture;
import com.android.internal.inputmethod.IInputMethodSession;
import com.android.internal.inputmethod.IRemoteInputConnection;
import com.android.internal.inputmethod.InputConnectionCommandHeader;
import com.android.internal.view.IInputMethodManager;

import com.gesher.server.InputMethodManagerService;
import com.gesher.server.StandIn;

import java.lang.ref.WeakReference;
import java.lang.reflect.Field;

/**
 * Windows' text input as the input method of a process's windows (docs/input.md, "Text
 * input"; ADR 0039). Windows' IMEs run in the process whose window has the focus; so does
 * this input method: it is {@link InputMethodManagerService}'s host in the process with the
 * Win32 windows (the window session's, ADR 0029).
 *
 * <ul>
 *   <li>Each window ({@link #addWindow}, from the window session) gets a session, an
 *       {@code IInputMethodSession} its InputMethodManager binds to when input starts
 *       ({@link #startInput}): the focused editor's {@code EditorInfo} and
 *       {@code IRemoteInputConnection} are the window's, the native side
 *       ({@link WindowInput#setTextInput}) learns whether an editor has the focus, whether it
 *       takes compositions (not a password or number field) and its Enter action.
 *   <li>The window's text events ({@link WindowInput.TextListener}, in order with its keys)
 *       go to the editor as an input method sends them: commitText, setComposingText (the
 *       IME's composition, its target clause highlighted, its caret), finishComposingText,
 *       performEditorAction. Each is finished once the editor has applied it, so the key
 *       events after it wait. Without an editor, typed text reaches the focused view as an
 *       {@code ACTION_MULTIPLE} KeyEvent, as before.
 *   <li>The editor's cursor (CursorAnchorInfo, requested with CURSOR_UPDATE_MONITOR) is the
 *       window's caret: the system caret, the IME's composition and candidate windows.
 *   <li>Key events reach the input method first (its InputChannel, as on a device), which
 *       hands them all back: the keys it takes are taken on the window's thread already.
 *   <li>Showing the soft input (InputMethodManager.showSoftInput: the window's requested
 *       visible types) shows Windows' touch keyboard when the editor was focused by touch or
 *       pen; hiding it hides the touch keyboard.
 * </ul>
 */
public final class WindowsInputMethod implements InputMethodManagerService.Host,
        WindowInput.TextListener {
    private static final String TAG = "gesher-ime";

    private static WindowsInputMethod sInstance;

    private final HandlerThread mThread = new HandlerThread("gesher-ime");
    private final Handler mHandler;
    private final InputChannel mClientChannel;
    private final InputEventReceiver mReceiver;
    private final ArrayMap<IBinder, Window> mWindows = new ArrayMap<>();

    /** A window of the process and its editor. */
    private final class Window extends IInputMethodSession.Stub {
        final IBinder token;
        WindowInput input;
        Handler handler;
        String title;
        long hwnd;
        // The editor (guarded by this).
        EditorInfo editor;
        IRemoteInputConnection connection;
        int sessionId;
        boolean active;
        boolean composing;
        int composeStart = -1;
        int selStart = -1;
        int selEnd = -1;
        int candStart = -1;
        int[] caret;
        boolean softInputShown;
        boolean touchKeyboardShown;
        int documentEpoch;
        int appliedRevision;
        int documentRequest;

        Window(IBinder token) {
            this.token = token;
        }

        // ---- IInputMethodSession (oneway; called with the client's InputMethodManager
        // locked: nothing here calls back into it) ----

        @Override
        public void updateExtractedText(int token, ExtractedText text) {
            // Updates may be partial and arrive under IMM's lock: request a
            // complete snapshot asynchronously instead of exposing a fragment.
            queueDocument(this);
        }

        @Override
        public void updateSelection(int oldSelStart, int oldSelEnd, int newSelStart,
                int newSelEnd, int candidatesStart, int candidatesEnd) {
            synchronized (this) {
                selStart = newSelStart;
                selEnd = newSelEnd;
                candStart = candidatesStart;
                if (candidatesStart >= 0) composeStart = candidatesStart;
            }
            queueDocument(this);
        }

        @Override
        public void viewClicked(boolean focusChanged) {
            // A click in the text while composing: Windows' IMEs commit the composition.
            WindowInput in;
            synchronized (this) {
                if (!composing || focusChanged) return;
                in = input;
            }
            if (in != null) in.completeComposition(false);
        }

        @Override
        public void updateCursor(android.graphics.Rect newCursor) {
        }

        @Override
        public void displayCompletions(CompletionInfo[] completions) {
        }

        @Override
        public void appPrivateCommand(String action, Bundle data) {
        }

        @Override
        public void finishSession() {
        }

        @Override
        public void updateCursorAnchorInfo(CursorAnchorInfo info) {
            Handler h;
            int epoch;
            synchronized (this) { h = handler; epoch = documentEpoch; }
            if (h != null && info != null) h.post(() -> applyCursorAnchorInfo(this, info, epoch));
        }

        @Override
        public void removeImeSurface() {
        }

        @Override
        public void finishInput() {
            WindowInput in;
            int epoch;
            synchronized (this) {
                active = false;
                connection = null;
                composing = false;
                documentEpoch++;
                appliedRevision = 0;
                in = input;
                epoch = documentEpoch;
            }
            if (in != null) in.setTextInput(false, false, 0, epoch);
        }

        @Override
        public void invalidateInput(EditorInfo editorInfo, IRemoteInputConnection ic,
                int newSessionId) {
            synchronized (this) {
                editor = editorInfo;
                connection = ic;
                sessionId = newSessionId;
                composing = false;
                documentEpoch++;
                appliedRevision = 0;
            }
            applyEditor(this);
        }
    }

    private WindowsInputMethod() {
        mThread.start();
        mHandler = new Handler(mThread.getLooper());
        InputChannel[] pair = InputChannel.openInputChannelPair("gesher-ime");
        mClientChannel = pair[1];
        // Key events come to the input method first (ImeInputStage); the keys it takes were
        // taken on the window's thread, so every one goes back to the window unhandled.
        mReceiver = new InputEventReceiver(pair[0], mThread.getLooper()) {
            @Override
            public void onInputEvent(InputEvent event) {
                finishInputEvent(event, false);
            }
        };
    }

    /**
     * The input method of this process's windows, installed as the host of its
     * InputMethodManagerService: the one Services made in this process (single process), or
     * one of its own that the process's InputMethodManagers talk to (an app process of the
     * Gesher device: the system server's instance has no windows). Before the first
     * InputMethodManager is made.
     */
    public static synchronized WindowsInputMethod install() {
        if (sInstance != null) return sInstance;
        WindowsInputMethod ime;
        try {
            ime = new WindowsInputMethod();
        } catch (RuntimeException | LinkageError e) {
            // No input natives (InputChannel): no input method; input_method still answers.
            log("no input method (" + e + ")");
            ime = null;
        }
        InputMethodManagerService service = InputMethodManagerService.inThisProcess();
        if (service == null) {
            service = new InputMethodManagerService();
            IInputMethodManager local = StandIn.of(IInputMethodManager.class, service)
                    .asInterface();
            if (ServiceManager.checkService(android.content.Context.INPUT_METHOD_SERVICE)
                    == null) {
                ServiceManager.addService(android.content.Context.INPUT_METHOD_SERVICE,
                        local.asBinder());
            }
            // The process's InputMethodManagers reach input_method through this cache.
            try {
                Class<?> invoker = Class.forName(
                        "android.view.inputmethod.IInputMethodManagerGlobalInvoker");
                Field f = invoker.getDeclaredField("sServiceCache");
                f.setAccessible(true);
                f.set(null, local);
            } catch (ReflectiveOperationException e) {
                log("IInputMethodManagerGlobalInvoker.sServiceCache: " + e);
            }
        }
        if (ime != null) service.setHost(ime);
        sInstance = ime;
        return ime;
    }

    /** The installed input method, or null. */
    public static synchronized WindowsInputMethod get() {
        return sInstance;
    }

    private static void log(String message) {
        System.out.println(TAG + ": " + message);
        System.out.flush();
    }

    /** Metadata only, enabled by the existing explicit TSF fixture environment. */
    private static void traceDocument(String message) {
        if (System.getenv("GESHER_TSF_TEST_SERVICE") != null) log("TSF document " + message);
    }

    /** {@code s} in ASCII, other characters as backslash-u escapes. */
    static String escape(CharSequence s) {
        if (s == null) return "null";
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c >= 0x20 && c < 0x7f) {
                out.append(c);
            } else {
                out.append(String.format("\\u%04x", (int) c));
            }
        }
        return out.toString();
    }

    // ---- the window session's windows ----

    /**
     * A window of the process got its input ({@code token}: its IWindow's binder;
     * {@code handler}: its ViewRootImpl's thread). Its text events come here from now on.
     */
    public void addWindow(IBinder token, WindowInput input, Handler handler, String title,
            long hwnd) {
        Window w;
        synchronized (mWindows) {
            w = mWindows.get(token);
            if (w == null) {
                w = new Window(token);
                mWindows.put(token, w);
            }
        }
        synchronized (w) {
            w.input = input;
            w.handler = handler;
            w.title = title;
            w.hwnd = hwnd;
        }
        input.setTextListener(this);
        applyEditor(w);
    }

    /** The window is going: before its input is closed. */
    public void removeWindow(IBinder token) {
        Window w;
        synchronized (mWindows) {
            w = mWindows.remove(token);
        }
        if (w == null) return;
        WindowInput in;
        boolean touch;
        synchronized (w) {
            in = w.input;
            touch = w.touchKeyboardShown;
            w.input = null;
            w.connection = null;
            w.active = false;
            w.touchKeyboardShown = false;
        }
        if (in != null) {
            if (touch) in.showTouchKeyboard(false);
            in.setTextListener(null);
        }
    }

    private Window window(IBinder token, boolean create) {
        synchronized (mWindows) {
            Window w = mWindows.get(token);
            if (w == null && create && token != null) {
                // A window without a Win32 window of its own (or not yet): a session all
                // the same.
                w = new Window(token);
                mWindows.put(token, w);
            }
            return w;
        }
    }

    private Window windowOf(WindowInput input) {
        synchronized (mWindows) {
            for (int i = 0; i < mWindows.size(); i++) {
                Window w = mWindows.valueAt(i);
                if (w.input == input) return w;
            }
        }
        return null;
    }

    // ---- InputMethodManagerService.Host ----

    @Override
    public InputMethodManagerService.Session startInput(IBinder windowToken,
            EditorInfo editorInfo, IRemoteInputConnection inputConnection, int startInputReason,
            int startInputFlags, int softInputMode) {
        Window w = window(windowToken, true);
        if (w == null) return null;
        synchronized (w) {
            w.editor = editorInfo;
            w.connection = inputConnection;
            w.sessionId = 0;
            w.documentEpoch++;
            w.appliedRevision = 0;
            w.composing = false;
            w.composeStart = -1;
            w.selStart = editorInfo != null ? editorInfo.initialSelStart : -1;
            w.selEnd = editorInfo != null ? editorInfo.initialSelEnd : -1;
            w.candStart = -1;
        }
        applyEditor(w);
        if (editorInfo != null && inputConnection != null) {
            log("input started in window \"" + w.title + "\": " + describe(editorInfo)
                    + ", keyboard layout " + Long.toHexString(keyboardLayout(windowToken)));
        }
        return new InputMethodManagerService.Session(w, mClientChannel.dup());
    }

    @Override
    public void onInputBound(IBinder windowToken) {
        Window w = window(windowToken, false);
        if (w == null) return;
        Handler h = w.handler != null ? w.handler : new Handler(Looper.getMainLooper());
        synchronized (w) {
            traceDocument("bound epoch " + w.documentEpoch + " input " + (w.input != null)
                    + " connection " + (w.connection != null));
        }
        // startInput's earlier query may precede InputMethodManager accepting its
        // result and return unavailable. Requery after that result is queued,
        // including an unchanged initial selection that emits no updateSelection.
        h.post(() -> { requestDocument(w); requestCursorUpdates(w); });
    }

    @Override
    public long keyboardLayout(IBinder windowToken) {
        Window w = window(windowToken, false);
        long hwnd;
        synchronized (w != null ? w : this) {
            hwnd = w != null ? w.hwnd : 0;
        }
        return hwnd != 0 ? WindowInput.keyboardLayout(hwnd) : 0;
    }

    // ---- the editor ----

    static String describe(EditorInfo e) {
        return "inputType 0x" + Integer.toHexString(e.inputType) + ", imeOptions 0x"
                + Integer.toHexString(e.imeOptions) + ", action " + enterAction(e)
                + ", compositions " + allowsCompositions(e) + ", package " + e.packageName
                + ", selection " + e.initialSelStart + "-" + e.initialSelEnd;
    }

    /** The IME action Enter performs in the editor (0: Enter stays a key). */
    static int enterAction(EditorInfo e) {
        if (e == null) return 0;
        if ((e.imeOptions & EditorInfo.IME_FLAG_NO_ENTER_ACTION) != 0) return 0;
        int cls = e.inputType & EditorInfo.TYPE_MASK_CLASS;
        if (cls == EditorInfo.TYPE_NULL) return 0;
        if (cls == EditorInfo.TYPE_CLASS_TEXT
                && (e.inputType & EditorInfo.TYPE_TEXT_FLAG_MULTI_LINE) != 0) {
            return 0;
        }
        int action = e.imeOptions & EditorInfo.IME_MASK_ACTION;
        return action == EditorInfo.IME_ACTION_NONE || action == EditorInfo.IME_ACTION_UNSPECIFIED
                ? 0 : action;
    }

    /** Whether IMEs may compose in the editor: text, not a password, not ASCII only. */
    static boolean allowsCompositions(EditorInfo e) {
        if (e == null) return false;
        if ((e.imeOptions & EditorInfo.IME_FLAG_FORCE_ASCII) != 0) return false;
        int cls = e.inputType & EditorInfo.TYPE_MASK_CLASS;
        if (cls != EditorInfo.TYPE_CLASS_TEXT) return false;
        int variation = e.inputType & EditorInfo.TYPE_MASK_VARIATION;
        return variation != EditorInfo.TYPE_TEXT_VARIATION_PASSWORD
                && variation != EditorInfo.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
                && variation != EditorInfo.TYPE_TEXT_VARIATION_WEB_PASSWORD;
    }

    /** The native side learns what the window's editor takes. */
    private void applyEditor(Window w) {
        WindowInput in;
        boolean active;
        boolean ime;
        int action;
        int epoch;
        synchronized (w) {
            in = w.input;
            active = w.connection != null && w.editor != null
                    && w.editor.inputType != EditorInfo.TYPE_NULL;
            ime = active && allowsCompositions(w.editor);
            action = active ? enterAction(w.editor) : 0;
            w.active = active;
            epoch = w.documentEpoch;
        }
        if (in != null) in.setTextInput(active, ime, action, epoch);
        queueDocument(w);
    }

    private void queueDocument(Window w) {
        Handler h = w.handler;
        if (h != null) h.post(() -> requestDocument(w));
    }

    private void requestDocument(Window w) {
        final IRemoteInputConnection ic;
        final InputConnectionCommandHeader command;
        final WindowInput input;
        final int epoch, revision, serial;
        synchronized (w) {
            input = w.input;
            if (input == null) {
                traceDocument("request skipped before native input attachment, epoch " + w.documentEpoch);
                return;
            }
            epoch = w.documentEpoch;
            revision = w.appliedRevision;
            serial = ++w.documentRequest;
            ic = w.active && allowsCompositions(w.editor) ? w.connection : null;
            command = header(w);
        }
        traceDocument("request epoch " + epoch + " revision " + revision + " serial " + serial
                + " session " + command.mSessionId + " connection " + (ic != null));
        if (ic == null) {
            input.setDocument("", 0, 0, 0, epoch, revision, false);
            return;
        }
        ExtractedTextRequest request = new ExtractedTextRequest();
        request.token = epoch;
        AndroidFuture<ExtractedText> reply = new AndroidFuture<>();
        reply.whenComplete((value, failure) -> {
            synchronized (w) {
                if (w.input != input || w.connection != ic || w.documentEpoch != epoch
                        || w.documentRequest != serial) {
                    traceDocument("stale reply epoch " + epoch + " serial " + serial
                            + " current epoch " + w.documentEpoch + " serial " + w.documentRequest);
                    return;
                }
                // selectionStart/End are relative to startOffset, per ExtractedText.
                boolean valid = failure == null && value != null && value.text != null
                        && value.partialStartOffset == -1 && value.startOffset >= 0
                        && value.selectionStart >= 0 && value.selectionEnd >= 0
                        && value.selectionStart <= value.text.length()
                        && value.selectionEnd <= value.text.length();
                traceDocument("reply epoch " + epoch + " revision " + revision + " serial " + serial
                        + " valid " + valid + " failure " + (failure != null ? failure.getClass().getName() : "none")
                        + " length " + (value != null && value.text != null ? value.text.length() : -1)
                        + " partial " + (value != null ? value.partialStartOffset : -2)
                        + " offset " + (value != null ? value.startOffset : -1)
                        + " selection " + (value != null ? value.selectionStart + "-" + value.selectionEnd : "none"));
                input.setDocument(valid ? value.text.toString() : "",
                        valid ? value.selectionStart : 0, valid ? value.selectionEnd : 0,
                        valid ? value.startOffset : 0, epoch, revision, valid);
            }
        });
        try {
            ic.getExtractedText(command, request, InputConnection.GET_EXTRACTED_TEXT_MONITOR, reply);
        } catch (RemoteException | RuntimeException e) { reply.completeExceptionally(e); }
    }

    /** Explicit fixture entry: the test service still uses the active real InputConnection. */
    public int runTextServiceTest(IBinder token) {
        Window w = window(token, false);
        WindowInput input;
        synchronized (w != null ? w : this) { input = w != null ? w.input : null; }
        return input != null ? input.runTextServiceTest() : -1;
    }

    private static InputConnectionCommandHeader header(Window w) {
        return new InputConnectionCommandHeader(w.sessionId);
    }

    private void requestCursorUpdates(Window w) {
        IRemoteInputConnection ic;
        InputConnectionCommandHeader h;
        synchronized (w) {
            ic = w.connection;
            h = header(w);
        }
        if (ic == null) return;
        try {
            ic.requestCursorUpdates(h, InputConnection.CURSOR_UPDATE_IMMEDIATE
                    | InputConnection.CURSOR_UPDATE_MONITOR, 0 /* the default display */,
                    new AndroidFuture<Boolean>());
        } catch (RemoteException e) {
            // The editor is gone.
        }
    }

    /** The editor's cursor as the window's caret (on the window's thread). */
    private void applyCursorAnchorInfo(Window w, CursorAnchorInfo info, int epoch) {
        WindowInput in;
        synchronized (w) {
            if (w.documentEpoch != epoch) return;
            in = w.input;
        }
        if (in == null) return;
        int[] origin = windowOrigin(w);
        CharSequence composingText = info.getComposingText();
        int compositionStart = info.getComposingTextStart();
        // A newer observation with no caret or character measurements must still
        // replace the previous range geometry. Native publication checks the epoch.
        in.setTextGeometry(epoch, Math.max(0, compositionStart),
                composingText != null && compositionStart >= 0 ? composingText.toString() : "",
                CursorAnchorGeometry.characters(info, origin[0], origin[1]));
        float x = info.getInsertionMarkerHorizontal();
        float top = info.getInsertionMarkerTop();
        float bottom = info.getInsertionMarkerBottom();
        if (Float.isNaN(x) || Float.isNaN(top) || Float.isNaN(bottom)) {
            // No insertion marker (a selection): the composition's or selection's first
            // character.
            int index = info.getComposingTextStart() >= 0 ? info.getComposingTextStart()
                    : info.getSelectionStart();
            RectF r = index >= 0 ? info.getCharacterBounds(index) : null;
            if (r == null) return;
            x = r.left;
            top = r.top;
            bottom = r.bottom;
        }
        float[] pts = {x, top, x, bottom};
        Matrix m = info.getMatrix();
        if (m != null) m.mapPoints(pts);
        for (float coordinate : pts) { if (!Float.isFinite(coordinate)) return; }
        // The matrix maps to the display; the Win32 window's client area is the Android
        // window's frame.
        int cx = Math.round(pts[0]) - origin[0];
        int cy = Math.round(pts[1]) - origin[1];
        int height = Math.max(1, Math.round(pts[3] - pts[1]));
        boolean changed;
        synchronized (w) {
            if (w.documentEpoch != epoch) return;
            in = w.input;
            int[] caret = {cx, cy, 2, height};
            changed = w.caret == null || !java.util.Arrays.equals(w.caret, caret);
            w.caret = caret;
        }
        if (in == null) return;
        in.setCaret(cx, cy, 2, height, epoch);
        if (changed) {
            log("caret in window \"" + w.title + "\" at " + cx + "," + cy + " height " + height);
        }
    }

    /** Where the window's frame is on the display (its root view's offset on screen). */
    private static int[] windowOrigin(Window w) {
        ViewRootImpl root = viewRoot(w.token);
        View v = root != null ? root.getView() : null;
        if (v == null) return new int[] {0, 0};
        int[] screen = new int[2];
        int[] window = new int[2];
        v.getLocationOnScreen(screen);
        v.getLocationInWindow(window);
        return new int[] {screen[0] - window[0], screen[1] - window[1]};
    }

    /** The window's ViewRootImpl (its IWindow is ViewRootImpl.W, in this process), or null. */
    private static ViewRootImpl viewRoot(IBinder token) {
        try {
            Field f = token.getClass().getDeclaredField("mViewAncestor");
            f.setAccessible(true);
            Object ref = f.get(token);
            return ref instanceof WeakReference
                    ? (ViewRootImpl) ((WeakReference<?>) ref).get() : null;
        } catch (ReflectiveOperationException | ClassCastException e) {
            return null;
        }
    }

    // ---- text events (WindowInput.TextListener, on the connection's thread) ----

    @Override
    public void onTextEvent(WindowInput input, int seq, int kind, String text, int cursor,
            int targetStart, int targetEnd) {
        Window w = windowOf(input);
        IRemoteInputConnection ic;
        InputConnectionCommandHeader h;
        boolean composing;
        int composeStart;
        synchronized (w != null ? w : this) {
            ic = w != null && w.active ? w.connection : null;
            h = w != null ? header(w) : null;
            composing = w != null && w.composing;
            composeStart = w != null ? w.composeStart : -1;
        }
        if (ic == null) {
            if (kind == WindowInput.TEXT_COMMIT && w != null) dispatchText(w, text);
            input.finishTextEvent(seq);
            return;
        }
        try {
            switch (kind) {
                case WindowInput.TEXT_COMMIT:
                    log("commit \"" + escape(text) + "\"" + (composing ? " (the composition)" : ""));
                    ic.commitText(h, text, 1);
                    synchronized (w) {
                        w.composing = false;
                        w.composeStart = -1;
                    }
                    break;
                case WindowInput.TEXT_COMPOSE:
                    if (text.isEmpty()) {
                        log("composition cancelled");
                        if (composing) {
                            ic.setComposingText(h, "", 1);
                            ic.finishComposingText(h);
                        }
                        synchronized (w) {
                            w.composing = false;
                            w.composeStart = -1;
                        }
                        break;
                    }
                    if (!composing) {
                        synchronized (w) {
                            composeStart = Math.min(w.selStart, w.selEnd);
                            w.composeStart = composeStart;
                        }
                    }
                    log("compose \"" + escape(text) + "\" caret " + cursor
                            + (targetStart >= 0 ? ", converting " + targetStart + "-" + targetEnd
                                    : ""));
                    SpannableString s = new SpannableString(text);
                    s.setSpan(new UnderlineSpan(), 0, text.length(),
                            Spanned.SPAN_EXCLUSIVE_EXCLUSIVE | Spanned.SPAN_COMPOSING);
                    if (targetStart >= 0 && targetEnd > targetStart && targetEnd <= text.length()) {
                        s.setSpan(new BackgroundColorSpan(0x603399ff), targetStart, targetEnd,
                                Spanned.SPAN_EXCLUSIVE_EXCLUSIVE | Spanned.SPAN_COMPOSING);
                    }
                    ic.setComposingText(h, s, 1);
                    if (cursor < text.length() && composeStart >= 0) {
                        ic.setSelection(h, composeStart + cursor, composeStart + cursor);
                    }
                    synchronized (w) {
                        w.composing = true;
                    }
                    break;
                case WindowInput.TEXT_COMPOSE_END:
                    if (composing) {
                        log("composition finished");
                        ic.finishComposingText(h);
                    }
                    synchronized (w) {
                        w.composing = false;
                        w.composeStart = -1;
                    }
                    break;
                case WindowInput.TEXT_EDITOR_ACTION:
                    log("editor action " + cursor + " (Enter)");
                    ic.performEditorAction(h, cursor);
                    break;
                default:
                    break;
            }
            // Finished once the editor has applied it: a query after it answers then.
            AndroidFuture<Integer> applied = new AndroidFuture<>();
            applied.whenComplete((r, e) -> { queueDocument(w); input.finishTextEvent(seq); });
            ic.getCursorCapsMode(h, 0, applied);
        } catch (RemoteException | RuntimeException e) {
            log("text event " + kind + ": " + e);
            input.finishTextEvent(seq);
        }
    }

    @Override
    public void onDocumentEdit(WindowInput input, int seq, int kind, String text,
            int epoch, int revision, int start, int end) {
        Window w = windowOf(input);
        IRemoteInputConnection ic;
        InputConnectionCommandHeader command;
        synchronized (w != null ? w : this) {
            if (w == null || w.documentEpoch != epoch || !w.active
                    || !allowsCompositions(w.editor) || revision <= w.appliedRevision) {
                input.finishTextEvent(seq);
                return;
            }
            ic = w.connection;
            command = header(w);
        }
        if (ic == null) { input.finishTextEvent(seq); return; }
        try {
            switch (kind) {
                case WindowInput.DOCUMENT_REPLACE:
                    ic.replaceText(command, start, end, text, 1, null); break;
                case WindowInput.DOCUMENT_COMPOSITION_TEXT:
                    // replaceText commits/removes composing spans. TSF text inside
                    // its active composition must reach the real composing API.
                    ic.setComposingRegion(command, start, end);
                    ic.setComposingText(command, text, 1);
                    synchronized (w) {
                        if (w.documentEpoch == epoch) { w.composing = true; w.composeStart = start; }
                    }
                    break;
                case WindowInput.DOCUMENT_SELECT:
                    ic.setSelection(command, start, end); break;
                case WindowInput.DOCUMENT_COMPOSITION_RANGE:
                    ic.setComposingRegion(command, start, end);
                    synchronized (w) {
                        if (w.documentEpoch == epoch) { w.composing = end > start; w.composeStart = start; }
                    }
                    break;
                case WindowInput.DOCUMENT_COMPOSITION_END:
                    log("commit \"" + escape(text) + "\"");
                    ic.finishComposingText(command);
                    synchronized (w) {
                        if (w.documentEpoch == epoch) { w.composing = false; w.composeStart = -1; }
                    }
                    break;
                default: input.finishTextEvent(seq); return;
            }
            AndroidFuture<Integer> applied = new AndroidFuture<>();
            applied.whenComplete((result, failure) -> {
                synchronized (w) {
                    if (failure == null && w.connection == ic && w.documentEpoch == epoch) {
                        w.appliedRevision = Math.max(w.appliedRevision, revision);
                    }
                }
                queueDocument(w);
                input.finishTextEvent(seq);
            });
            ic.getCursorCapsMode(command, 0, applied);
        } catch (RemoteException | RuntimeException e) {
            log("document edit " + kind + ": " + e);
            input.finishTextEvent(seq);
        }
    }

    /** Text with no editor: an ACTION_MULTIPLE KeyEvent to the window's focused view. */
    private static void dispatchText(Window w, String text) {
        Handler h = w.handler;
        if (h == null || text == null || text.isEmpty()) return;
        h.post(() -> {
            ViewRootImpl root = viewRoot(w.token);
            if (root == null) return;
            root.dispatchInputEvent(new KeyEvent(SystemClock.uptimeMillis(), text,
                    KeyCharacterMap.VIRTUAL_KEYBOARD, 0));
        });
    }

    // ---- the soft input ----

    /**
     * The window's requested visible types changed (the window session's
     * updateRequestedVisibleTypes): InputMethodManager.showSoftInput and
     * hideSoftInputFromWindow, through the window's insets. The touch keyboard is shown when
     * the editor was focused by a finger or a pen, as Windows does for its own text fields.
     */
    public void onRequestedVisibleTypes(IBinder token, int types) {
        Window w = window(token, false);
        if (w == null) return;
        boolean visible = (types & WindowInsets.Type.ime()) != 0;
        WindowInput in;
        boolean touch;
        synchronized (w) {
            if (visible == w.softInputShown) return;
            w.softInputShown = visible;
            in = w.input;
            int tool = w.editor != null ? w.editor.getInitialToolType() : 0;
            touch = tool == MotionEvent.TOOL_TYPE_FINGER || tool == MotionEvent.TOOL_TYPE_STYLUS;
        }
        String what = "soft input " + (visible ? "shown" : "hidden") + " in window \""
                + w.title + "\"";
        if (in == null) {
            log(what);
            return;
        }
        if (visible && !touch) {
            log(what + " (keyboard focus: no touch keyboard)");
            return;
        }
        boolean shown;
        synchronized (w) {
            shown = w.touchKeyboardShown;
        }
        if (!visible && !shown) {
            log(what);
            return;
        }
        final WindowInput target = in;
        mHandler.post(() -> {
            int r = target.showTouchKeyboard(visible);
            synchronized (w) {
                w.touchKeyboardShown = visible && r == 1;
            }
            log(what + ": touch keyboard " + (visible ? "TryShow " : "TryHide ")
                    + (r == 1 ? "done" : r == 0 ? "declined" : "0x" + Integer.toHexString(r)));
        });
    }
}
