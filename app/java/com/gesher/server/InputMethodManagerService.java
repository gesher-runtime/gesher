/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */
package com.gesher.server;

import android.content.pm.ApplicationInfo;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.ResultReceiver;
import android.view.InputChannel;
import android.view.inputmethod.CursorAnchorInfo;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodInfo;
import android.view.inputmethod.InputMethodSubtype;

import com.android.internal.inputmethod.IInputMethodClient;
import com.android.internal.inputmethod.IInputMethodSession;
import com.android.internal.inputmethod.IRemoteAccessibilityInputConnection;
import com.android.internal.inputmethod.IRemoteComputerControlInputConnection;
import com.android.internal.inputmethod.IRemoteInputConnection;
import com.android.internal.inputmethod.InputBindResult;
import com.android.internal.inputmethod.InputMethodInfoSafeList;
import com.android.internal.inputmethod.StartInputFlags;
import com.android.internal.view.IInputMethodManager;

import com.gesher.app.input.WindowInput;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.WeakHashMap;

/**
 * input_method: InputMethodManagerService with one input method, Windows' own text input
 * (docs/input.md, "Text input"; ADR 0039). Its subtypes are the session's keyboard layouts
 * (input languages); the current one is the focused window's thread's layout.
 *
 * <p>The input method runs where the windows are, as Windows' IMEs run in the process whose
 * window has the focus: the {@link Host} of the process (com.gesher.app.input.
 * WindowsInputMethod, installed with the window session) gives each window a session, an
 * {@code IInputMethodSession} its InputMethodManager talks to, and turns the Win32 window's
 * text and IME compositions into calls on the focused editor's {@code IRemoteInputConnection}.
 * The system server's instance (no windows, no host) answers the input method list and
 * subtypes; a process with windows serves its own clients with an instance of its own,
 * bound to its host (WindowsInputMethod.install).
 *
 * <p>{@code startInputOrWindowGainedFocus} binds the client to the window's session
 * ({@code onStartInputResult} with SUCCESS_WITH_IME_SESSION) and makes the client active;
 * showing and hiding the soft input go through the window's insets (the window session's
 * {@code updateRequestedVisibleTypes}) on Android 16, not through this service.
 */
public final class InputMethodManagerService extends IInputMethodManager.Default {
    /** The input method's package and class (its id is their short component name). */
    public static final String IME_PACKAGE = "com.gesher.app";
    public static final String IME_CLASS = "com.gesher.app.input.WindowsInputMethod";
    public static final String IME_ID = IME_PACKAGE + "/.input.WindowsInputMethod";
    /** The subtypes' mode (InputMethodSubtype.getMode). */
    public static final String SUBTYPE_MODE = "keyboard";

    /** The input method of the process's windows. */
    public interface Host {
        /**
         * Input starts in window {@code windowToken} (an IWindow's binder) of this process.
         * {@code editorInfo} is the focused editor's; a focus-only report with no editor
         * does not start or replace a host binding. Returns the session the
         * window's client talks to, or null. Called on the client's thread with its
         * InputMethodManager locked: the host posts its work, it does not call back.
         */
        Session startInput(IBinder windowToken, EditorInfo editorInfo,
                IRemoteInputConnection inputConnection, int startInputReason,
                int startInputFlags, int softInputMode);

        /** The client's binding result has been enqueued; host requests may follow it. */
        default void onInputBound(IBinder windowToken) { }

        /** The keyboard layout (HKL) of the thread of window {@code windowToken}; 0: unknown. */
        long keyboardLayout(IBinder windowToken);
    }

    /** What a client binds to: the input method's session and its input event channel. */
    public static final class Session {
        public final IInputMethodSession session;
        /** The client's end of the channel key events reach the input method through. */
        public final InputChannel channel;

        public Session(IInputMethodSession session, InputChannel channel) {
            this.session = session;
            this.channel = channel;
        }
    }

    private static final class Client {
        final IInputMethodClient client;
        boolean active;

        Client(IInputMethodClient client) {
            this.client = client;
        }
    }

    private static volatile InputMethodManagerService sInProcess;

    private final Map<IBinder, Client> mClients = new WeakHashMap<>();
    private volatile Host mHost;
    private Client mCurClient;
    private IBinder mCurWindow;
    private int mBindSequence;
    private List<InputMethodSubtype> mSubtypes;
    private InputMethodInfo mInfo;

    public InputMethodManagerService() {
        sInProcess = this;
    }

    /** The instance made last in this process (by Services.create), or null. */
    public static InputMethodManagerService inThisProcess() {
        return sInProcess;
    }

    /** The input method of this process's windows. */
    public void setHost(Host host) {
        mHost = host;
    }

    // ---- the input method ----

    /** The session's keyboard layouts as subtypes (en-US when none can be read). */
    private synchronized List<InputMethodSubtype> subtypes() {
        if (mSubtypes != null) return mSubtypes;
        List<InputMethodSubtype> out = new ArrayList<>();
        String[][] layouts;
        try {
            layouts = WindowInput.keyboardLayouts();
        } catch (LinkageError e) {
            layouts = new String[0][];
        }
        for (String[] l : layouts) {
            long hkl = Long.parseUnsignedLong(l[0], 16);
            String locale = l[1].isEmpty() ? "en-US" : l[1];
            out.add(subtype(hkl, locale, l[2].isEmpty() ? locale : l[2]));
        }
        if (out.isEmpty()) out.add(subtype(0x04090409L, "en-US", "English (United States)"));
        mSubtypes = Collections.unmodifiableList(out);
        return mSubtypes;
    }

    private static InputMethodSubtype subtype(long hkl, String locale, String name) {
        return new InputMethodSubtype.InputMethodSubtypeBuilder()
                .setSubtypeId((int) hkl)
                .setSubtypeLocale(locale.replace('-', '_'))
                .setLanguageTag(locale)
                .setSubtypeMode(SUBTYPE_MODE)
                .setSubtypeNameOverride(name)
                .setSubtypeExtraValue("KeyboardLayout=" + Long.toHexString(hkl))
                .setIsAsciiCapable(true)
                .build();
    }

    private synchronized InputMethodInfo info() {
        if (mInfo != null) return mInfo;
        ResolveInfo ri = new ResolveInfo();
        ServiceInfo si = new ServiceInfo();
        ApplicationInfo ai = new ApplicationInfo();
        ai.packageName = IME_PACKAGE;
        ai.enabled = true;
        si.applicationInfo = ai;
        si.enabled = true;
        si.packageName = IME_PACKAGE;
        si.name = IME_CLASS;
        si.exported = true;
        si.nonLocalizedLabel = "Windows text input";
        ri.serviceInfo = si;
        mInfo = new InputMethodInfo(ri, false /* isAuxIme */, null /* settingsActivity */,
                subtypes(), 0 /* isDefaultResId */, true /* forceDefault */);
        return mInfo;
    }

    private InputMethodSubtype currentSubtype() {
        Host host = mHost;
        IBinder window;
        synchronized (this) {
            window = mCurWindow;
        }
        long hkl = host != null && window != null ? host.keyboardLayout(window) : 0;
        if (hkl == 0) {
            try {
                hkl = WindowInput.keyboardLayout(0);
            } catch (LinkageError e) {
                hkl = 0;
            }
        }
        List<InputMethodSubtype> subtypes = subtypes();
        for (InputMethodSubtype s : subtypes) {
            if (s.getExtraValue().equals("KeyboardLayout=" + Long.toHexString(hkl))) {
                return s;
            }
        }
        // A layout loaded since the list was read: read it again.
        synchronized (this) {
            mSubtypes = null;
            mInfo = null;
        }
        for (InputMethodSubtype s : subtypes()) {
            if (s.getExtraValue().equals("KeyboardLayout=" + Long.toHexString(hkl))) {
                return s;
            }
        }
        return subtypes().get(0);
    }

    // ---- IInputMethodManager ----

    @Override
    public void addClient(IInputMethodClient client, IRemoteInputConnection inputConnection,
            int untrustedDisplayId) {
        synchronized (this) {
            mClients.put(client.asBinder(), new Client(client));
        }
    }

    @Override
    public void startInputOrWindowGainedFocus(int startInputReason, IInputMethodClient client,
            IBinder windowToken, int startInputFlags, int softInputMode, int windowFlags,
            EditorInfo editorInfo, IRemoteInputConnection inputConnection,
            IRemoteAccessibilityInputConnection remoteAccessibilityInputConnection,
            IRemoteComputerControlInputConnection remoteComputerControlInputConnection,
            int unverifiedTargetSdkVersion, int userId, ResultReceiver imeBackCallbackReceiver,
            boolean imeRequestedVisible, int startInputSeq) {
        Client c;
        Client previous;
        int sequence;
        boolean sameWindowFocused;
        synchronized (this) {
            c = mClients.get(client.asBinder());
            if (c == null) {
                // A client of another service instance (made before this one was installed).
                c = new Client(client);
                mClients.put(client.asBinder(), c);
            }
            previous = mCurClient;
            mCurClient = c;
            sameWindowFocused = windowToken != null && mCurWindow == windowToken;
            if (windowToken != null) mCurWindow = windowToken;
            sequence = ++mBindSequence;
        }
        try {
            if (previous != null && previous != c && previous.active) {
                previous.active = false;
                previous.client.setActive(false, false);
            }
            if (!c.active) {
                c.active = true;
                c.client.setActive(true, false);
            }
        } catch (RemoteException e) {
            // The client is gone.
        }
        Host host = mHost;
        InputBindResult result;
        boolean bound = false;
        if (editorInfo == null) {
            // AOSP16 IMM also reports WINDOW_FOCUS_GAIN_REPORT_ONLY after an
            // editor has started. Its null editor/connection are a notification,
            // not a request to erase that editor's live native document.
            result = sameWindowFocused && (startInputFlags & StartInputFlags.IS_TEXT_EDITOR) != 0
                    ? new InputBindResult(InputBindResult.ResultCode.SUCCESS_REPORT_WINDOW_FOCUS_ONLY,
                            null, null, null, null, -1, false)
                    : InputBindResult.NULL_EDITOR_INFO;
            if (System.getenv("GESHER_TSF_TEST_SERVICE") != null) {
                System.out.println("gesher-ime: focus-only report without a new editor binding, reason "
                        + startInputReason + " flags 0x" + Integer.toHexString(startInputFlags));
            }
        } else if (host == null) {
            result = InputBindResult.NO_IME;
        } else {
            Session s = host.startInput(windowToken, editorInfo, inputConnection,
                    startInputReason, startInputFlags, softInputMode);
            bound = s != null;
            result = s == null ? InputBindResult.NO_EDITOR
                    : new InputBindResult(InputBindResult.ResultCode.SUCCESS_WITH_IME_SESSION,
                            s.session, null /* accessibilitySessions */, s.channel, IME_ID,
                            sequence, false /* isInputMethodSuppressingSpellChecker */);
        }
        try {
            client.onStartInputResult(result, startInputSeq);
            if (bound) host.onInputBound(windowToken);
        } catch (RemoteException e) {
            // The client is gone.
        }
    }

    @Override
    public InputMethodInfo getCurrentInputMethodInfoAsUser(int userId) {
        return info();
    }

    @Override
    public InputMethodInfoSafeList getInputMethodList(int userId, int directBootAwareness) {
        return InputMethodInfoSafeList.create(List.of(info()));
    }

    @Override
    public InputMethodInfoSafeList getEnabledInputMethodList(int userId) {
        return InputMethodInfoSafeList.create(List.of(info()));
    }

    @Override
    public List<InputMethodInfo> getInputMethodListLegacy(int userId, int directBootAwareness) {
        return List.of(info());
    }

    @Override
    public List<InputMethodInfo> getEnabledInputMethodListLegacy(int userId) {
        return List.of(info());
    }

    @Override
    public List<InputMethodSubtype> getEnabledInputMethodSubtypeList(String imiId,
            boolean allowsImplicitlyEnabledSubtypes, int userId) {
        if (imiId != null && !imiId.equals(IME_ID)) return Collections.emptyList();
        return new ArrayList<>(subtypes());
    }

    @Override
    public InputMethodSubtype getLastInputMethodSubtype(int userId) {
        return currentSubtype();
    }

    @Override
    public InputMethodSubtype getCurrentInputMethodSubtype(int userId) {
        return currentSubtype();
    }

    @Override
    public void setAdditionalInputMethodSubtypes(String id, InputMethodSubtype[] subtypes,
            int userId) {
        // Windows' input languages are the subtypes; there are no others.
    }

    @Override
    public void setExplicitlyEnabledInputMethodSubtypes(String imeId, int[] subtypeHashCodes,
            int userId) {
    }

    @Override
    public void showInputMethodPickerFromClient(IInputMethodClient client,
            int auxiliarySubtypeMode) {
        // Windows switches input languages itself (Win+Space, the taskbar's indicator).
    }

    @Override
    public void showInputMethodPickerFromSystem(int auxiliarySubtypeMode, int displayId) {
    }

    @Override
    public int getInputMethodWindowVisibleHeight(IInputMethodClient client) {
        return 0;  // Windows' IME windows and the touch keyboard float over the window
    }

    @Override
    public void reportPerceptible(IBinder windowToken, boolean perceptible) {
    }

    @Override
    public void removeImeSurfaceFromWindow(IBinder windowToken) {
    }

    @Override
    public boolean isImeTraceEnabled() {
        return false;
    }

    @Override
    public boolean isStylusHandwritingAvailableAsUser(int userId, boolean connectionless) {
        return false;
    }

    @Override
    public void startStylusHandwriting(IInputMethodClient client) {
    }

    @Override
    public void startConnectionlessStylusHandwriting(IInputMethodClient client, int userId,
            CursorAnchorInfo cursorAnchorInfo, String delegatePackageName,
            String delegatorPackageName,
            com.android.internal.inputmethod.IConnectionlessHandwritingCallback callback) {
    }

    @Override
    public void prepareStylusHandwritingDelegation(IInputMethodClient client, int userId,
            String delegatePackageName, String delegatorPackageName) {
    }

    @Override
    public boolean acceptStylusHandwritingDelegation(IInputMethodClient client, int userId,
            String delegatePackageName, String delegatorPackageName, int flags) {
        return false;
    }

    @Override
    public void acceptStylusHandwritingDelegationAsync(IInputMethodClient client, int userId,
            String delegatePackageName, String delegatorPackageName, int flags,
            com.android.internal.inputmethod.IBooleanListener callback) {
        try {
            callback.onResult(false);
        } catch (RemoteException e) {
            // gone
        }
    }
}
