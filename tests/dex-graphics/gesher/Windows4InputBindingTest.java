package gesher;

import android.os.Binder;
import android.os.IBinder;
import android.text.InputType;
import android.view.inputmethod.EditorInfo;
import com.android.internal.inputmethod.IInputMethodClient;
import com.android.internal.inputmethod.IInputMethodSession;
import com.android.internal.inputmethod.IRemoteInputConnection;
import com.android.internal.inputmethod.InputBindResult;
import com.android.internal.inputmethod.StartInputFlags;
import com.android.internal.inputmethod.StartInputReason;
import com.gesher.server.InputMethodManagerService;

/** Real service binding contract under ART; the full TsfActivity verifies the live editor. */
public final class Windows4InputBindingTest {
    private static int failed;

    private static void check(boolean okay, String description) {
        System.out.println((okay ? "PASS" : "FAIL") + " windows4 input binding: " + description);
        if (!okay) failed++;
    }

    private static final class Client extends IInputMethodClient.Default {
        final IBinder token = new Binder();
        InputBindResult result;
        @Override public IBinder asBinder() { return token; }
        @Override public void onStartInputResult(InputBindResult value, int sequence) { result = value; }
    }

    private static final class Host implements InputMethodManagerService.Host {
        int starts, bound;
        EditorInfo editor;
        IRemoteInputConnection connection;
        @Override public InputMethodManagerService.Session startInput(IBinder window, EditorInfo info,
                IRemoteInputConnection input, int reason, int flags, int mode) {
            starts++;
            editor = info;
            connection = input;
            return new InputMethodManagerService.Session(new IInputMethodSession.Default(), null);
        }
        @Override public void onInputBound(IBinder window) { bound++; }
        @Override public long keyboardLayout(IBinder window) { return 0; }
    }

    private static void start(InputMethodManagerService service, Client client, IBinder window,
            EditorInfo editor, IRemoteInputConnection connection, int flags, int sequence) {
        service.startInputOrWindowGainedFocus(editor != null ? StartInputReason.APP_CALLED_RESTART_INPUT_API
                        : StartInputReason.WINDOW_FOCUS_GAIN_REPORT_ONLY,
                client, window, flags, 0, 0, editor, connection, null, null, 36, 0, null, false, sequence);
    }

    public static void main(String[] args) {
        System.loadLibrary("android_runtime");
        InputMethodManagerService service = new InputMethodManagerService();
        Host host = new Host();
        Client client = new Client();
        IBinder window = new Binder(), other = new Binder();
        EditorInfo editor = new EditorInfo();
        editor.inputType = InputType.TYPE_CLASS_TEXT;
        editor.initialSelStart = 7;
        editor.initialSelEnd = 11;
        IRemoteInputConnection connection = new IRemoteInputConnection.Default();
        int flags = StartInputFlags.VIEW_HAS_FOCUS | StartInputFlags.IS_TEXT_EDITOR
                | StartInputFlags.WINDOW_GAINED_FOCUS;
        service.setHost(host);
        start(service, client, window, editor, connection, flags, 1);
        check(host.starts == 1 && host.bound == 1 && host.editor == editor && host.connection == connection
                        && client.result.result == InputBindResult.ResultCode.SUCCESS_WITH_IME_SESSION,
                "a real editor starts and binds its input session");

        start(service, client, window, null, null, flags, 2);
        check(host.starts == 1 && host.bound == 1 && host.editor == editor && host.connection == connection
                        && client.result.result == InputBindResult.ResultCode.SUCCESS_REPORT_WINDOW_FOCUS_ONLY,
                "a later same-window focus-only report preserves the existing editor and connection");
        start(service, client, other, null, null, StartInputFlags.WINDOW_GAINED_FOCUS, 3);
        check(host.starts == 1 && host.bound == 1 && host.connection == connection
                        && client.result.result == InputBindResult.ResultCode.ERROR_NULL_EDITOR_INFO,
                "a new window without an editor does not replace a host editor binding");

        IRemoteInputConnection replacement = new IRemoteInputConnection.Default();
        start(service, client, window, editor, replacement, flags, 4);
        check(host.starts == 2 && host.bound == 2 && host.connection == replacement,
                "a subsequent real restart replaces the connection and binds normally");
        start(service, client, window, editor, null, flags, 5);
        check(host.starts == 3 && host.bound == 3 && host.connection == null,
                "a real editor update without a connection is still delivered to the host");

        InputMethodManagerService headless = new InputMethodManagerService();
        Client headlessClient = new Client();
        start(headless, headlessClient, new Binder(), null, null, 0, 1);
        check(headlessClient.result.result == InputBindResult.ResultCode.ERROR_NULL_EDITOR_INFO,
                "a headless focus-only report keeps the null-editor result");
        start(headless, headlessClient, new Binder(), editor, connection, flags, 2);
        check(headlessClient.result.result == InputBindResult.ResultCode.ERROR_NO_IME,
                "a headless real editor keeps the no-IME result");
        if (failed != 0) throw new AssertionError(failed + " input binding check(s) failed");
        System.out.println("Windows4InputBindingTest all passed");
    }
}
