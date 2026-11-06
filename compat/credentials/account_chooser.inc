/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
/* Included by account_crypto.c: shares its existing system-server role guard. */

enum {
  ACCOUNT_CHOOSER_MAX_LABELS = 64,
  ACCOUNT_CHOOSER_MAX_LABEL_CHARS = 2048,
  ACCOUNT_CHOOSER_MAX_PACKAGE_CHARS = 1024,
  ACCOUNT_CHOOSER_LIST = 1001,
  ACCOUNT_CHOOSER_ADD = 1002,
  ACCOUNT_CHOOSER_TIMER = 1
};

typedef struct {
  JNIEnv* env;
  jobject is_active;
  jmethodID active_method;
  DWORD thread;
  BOOL can_add;
  BOOL ended;
  jsize count;
  jint result;
  WCHAR* package_name;
  SIZE_T package_bytes;
  WCHAR* labels[ACCOUNT_CHOOSER_MAX_LABELS];
  SIZE_T label_bytes[ACCOUNT_CHOOSER_MAX_LABELS];
} AccountChooserState;

static void AccountChooserFree(AccountChooserState* state) {
  for (jsize i = 0; i < state->count; ++i) {
    if (state->labels[i] != NULL) {
      SecureZeroMemory(state->labels[i], state->label_bytes[i]);
      HeapFree(GetProcessHeap(), 0, state->labels[i]);
    }
  }
  if (state->package_name != NULL) {
    SecureZeroMemory(state->package_name, state->package_bytes);
    HeapFree(GetProcessHeap(), 0, state->package_name);
  }
  SecureZeroMemory(state, sizeof(*state));
}

static WCHAR* AccountChooserCopy(JNIEnv* env, jstring text, jsize maximum, SIZE_T* bytes) {
  if (text == NULL) {
    Fail(env, "java/lang/IllegalArgumentException", "Missing account chooser text");
    return NULL;
  }
  jsize length = (*env)->GetStringLength(env, text);
  if (length <= 0 || length > maximum) {
    Fail(env, "java/lang/IllegalArgumentException", "Invalid account chooser text length");
    return NULL;
  }
  *bytes = ((SIZE_T)length + 1) * sizeof(WCHAR);
  WCHAR* copy = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, *bytes);
  if (copy == NULL) {
    Fail(env, "java/lang/OutOfMemoryError", "Account chooser allocation failed");
    return NULL;
  }
  (*env)->GetStringRegion(env, text, 0, length, (jchar*)copy);
  BOOL valid = !(*env)->ExceptionCheck(env);
  for (jsize i = 0; valid && i < length; ++i) valid = copy[i] != 0;
  if (!valid) {
    SecureZeroMemory(copy, *bytes);
    HeapFree(GetProcessHeap(), 0, copy);
    if (!(*env)->ExceptionCheck(env)) {
      Fail(env, "java/lang/IllegalArgumentException", "Invalid account chooser text");
    }
    return NULL;
  }
  return copy;
}

/* The modal dialog pumps this same JNI thread. No JNIEnv or local reference
 * crosses a thread boundary, and no JVM reference survives nativeChoose. */
static BOOL AccountChooserActive(AccountChooserState* state) {
  if (GetCurrentThreadId() != state->thread) return FALSE;
  JNIEnv* env = state->env;
  if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return FALSE; }
  jboolean active = (*env)->CallBooleanMethod(env, state->is_active, state->active_method);
  if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return FALSE; }
  return active == JNI_TRUE;
}

static void AccountChooserFinish(HWND dialog, AccountChooserState* state, jint result) {
  if (state->ended) return;
  state->ended = TRUE;
  state->result = result;
  KillTimer(dialog, ACCOUNT_CHOOSER_TIMER);
  HWND list = GetDlgItem(dialog, ACCOUNT_CHOOSER_LIST);
  if (list != NULL) SendMessageW(list, LB_RESETCONTENT, 0, 0);
  EndDialog(dialog, result);
}

static HWND AccountChooserControl(HWND dialog, DWORD extra, const WCHAR* klass,
        const WCHAR* text, DWORD style, int id, int x, int y, int width, int height) {
  RECT bounds = {x, y, x + width, y + height};
  if (!MapDialogRect(dialog, &bounds)) return NULL;
  HWND control = CreateWindowExW(extra, klass, text, WS_CHILD | WS_VISIBLE | style,
      bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
      dialog, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
  if (control != NULL) {
    SendMessageW(control, WM_SETFONT, SendMessageW(dialog, WM_GETFONT, 0, 0), FALSE);
  }
  return control;
}

/* Windows invokes this procedure with its host CALLBACK ABI, not GESHER_NATIVE. */
static INT_PTR CALLBACK AccountChooserDialog(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
  AccountChooserState* state = (AccountChooserState*)GetWindowLongPtrW(dialog, DWLP_USER);
  if (message == WM_INITDIALOG) {
    state = (AccountChooserState*)lparam;
    SetLastError(0);
    LONG_PTR previous = SetWindowLongPtrW(dialog, DWLP_USER, (LONG_PTR)state);
    if ((previous == 0 && GetLastError() != 0) || !AccountChooserActive(state)) {
      AccountChooserFinish(dialog, state, -1);
      return FALSE;
    }
    HWND prompt = AccountChooserControl(dialog, 0, L"STATIC",
        L"Choose an account for this application:", SS_LEFT, -1, 10, 8, 280, 12);
    HWND package = AccountChooserControl(dialog, 0, L"STATIC", state->package_name,
        SS_LEFT | SS_ENDELLIPSIS, -1, 10, 23, 280, 18);
    HWND list = AccountChooserControl(dialog, WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        ACCOUNT_CHOOSER_LIST, 10, 46, 280, 122);
    HWND select = AccountChooserControl(dialog, 0, L"BUTTON", L"Select",
        WS_TABSTOP | BS_PUSHBUTTON, IDOK, 124, 184, 78, 16);
    HWND cancel = AccountChooserControl(dialog, 0, L"BUTTON", L"Cancel",
        WS_TABSTOP | BS_DEFPUSHBUTTON, IDCANCEL, 212, 184, 78, 16);
    HWND add = state->can_add ? AccountChooserControl(dialog, 0, L"BUTTON", L"Add account",
        WS_TABSTOP | BS_PUSHBUTTON, ACCOUNT_CHOOSER_ADD, 10, 184, 98, 16) : NULL;
    if (prompt == NULL || package == NULL || list == NULL || select == NULL
        || cancel == NULL || (state->can_add && add == NULL)) {
      AccountChooserFinish(dialog, state, -1);
      return FALSE;
    }
    for (jsize i = 0; i < state->count; ++i) {
      LRESULT added = SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)state->labels[i]);
      if (added == LB_ERR || added == LB_ERRSPACE) {
        AccountChooserFinish(dialog, state, -1);
        return FALSE;
      }
    }
    SendMessageW(list, LB_SETCURSEL, (WPARAM)-1, 0);
    EnableWindow(select, FALSE);
    SendMessageW(dialog, DM_SETDEFID, IDCANCEL, 0);
    if (SetTimer(dialog, ACCOUNT_CHOOSER_TIMER, 250, NULL) == 0) {
      AccountChooserFinish(dialog, state, -1);
      return FALSE;
    }
    SetFocus(cancel);
    return FALSE;
  }
  if (state == NULL) return FALSE;
  if (message == WM_TIMER && wparam == ACCOUNT_CHOOSER_TIMER) {
    if (!AccountChooserActive(state)) AccountChooserFinish(dialog, state, -1);
    return TRUE;
  }
  if (message == WM_CLOSE) {
    AccountChooserFinish(dialog, state, -1);
    return TRUE;
  }
  if (message == WM_DESTROY) {
    KillTimer(dialog, ACCOUNT_CHOOSER_TIMER);
    SetWindowLongPtrW(dialog, DWLP_USER, 0);
    return TRUE;
  }
  if (message != WM_COMMAND) return FALSE;
  int command = LOWORD(wparam);
  if (command == IDCANCEL) {
    AccountChooserFinish(dialog, state, -1);
    return TRUE;
  }
  if (command == ACCOUNT_CHOOSER_LIST && HIWORD(wparam) == LBN_SELCHANGE) {
    if (!AccountChooserActive(state)) { AccountChooserFinish(dialog, state, -1); return TRUE; }
    LRESULT selected = SendDlgItemMessageW(dialog, ACCOUNT_CHOOSER_LIST, LB_GETCURSEL, 0, 0);
    EnableWindow(GetDlgItem(dialog, IDOK), selected >= 0 && selected < state->count);
    return TRUE;
  }
  if (command == IDOK || command == ACCOUNT_CHOOSER_ADD) {
    if (!AccountChooserActive(state)) { AccountChooserFinish(dialog, state, -1); return TRUE; }
    if (command == ACCOUNT_CHOOSER_ADD) {
      if (state->can_add) AccountChooserFinish(dialog, state, -2);
    } else {
      if (!IsWindowEnabled(GetDlgItem(dialog, IDOK))) return TRUE;
      LRESULT selected = SendDlgItemMessageW(dialog, ACCOUNT_CHOOSER_LIST, LB_GETCURSEL, 0, 0);
      if (selected >= 0 && selected < state->count) AccountChooserFinish(dialog, state, (jint)selected);
    }
    return TRUE;
  }
  return FALSE;
}

JNIEXPORT jint GESHER_NATIVE
Java_com_gesher_server_AccountChooser_nativeChoose(JNIEnv* env, jclass cls,
        jstring package_name, jobjectArray labels, jboolean can_add, jobject is_active) {
  (void)cls;
  if (!RequireServer(env)) return -1;
  if (package_name == NULL || labels == NULL || is_active == NULL) {
    Fail(env, "java/lang/IllegalArgumentException", "Missing account chooser input");
    return -1;
  }
  jsize count = (*env)->GetArrayLength(env, labels);
  if (count < 0 || count > ACCOUNT_CHOOSER_MAX_LABELS) {
    Fail(env, "java/lang/IllegalArgumentException", "Too many account chooser rows");
    return -1;
  }
  jclass supplier = (*env)->FindClass(env, "java/util/function/BooleanSupplier");
  if (supplier == NULL) return -1;
  if (!(*env)->IsInstanceOf(env, is_active, supplier)) {
    (*env)->DeleteLocalRef(env, supplier);
    Fail(env, "java/lang/IllegalArgumentException", "Invalid account chooser lifetime predicate");
    return -1;
  }
  jmethodID active_method = (*env)->GetMethodID(env, supplier, "getAsBoolean", "()Z");
  (*env)->DeleteLocalRef(env, supplier);
  if (active_method == NULL) return -1;
  AccountChooserState state = {0};
  state.env = env;
  state.is_active = is_active;
  state.active_method = active_method;
  state.thread = GetCurrentThreadId();
  state.count = count;
  state.can_add = can_add == JNI_TRUE;
  state.result = -1;
  state.package_name = AccountChooserCopy(env, package_name, ACCOUNT_CHOOSER_MAX_PACKAGE_CHARS,
      &state.package_bytes);
  if (state.package_name == NULL) { AccountChooserFree(&state); return -1; }
  for (jsize i = 0; i < count; ++i) {
    jstring label = (jstring)(*env)->GetObjectArrayElement(env, labels, i);
    if ((*env)->ExceptionCheck(env)) { AccountChooserFree(&state); return -1; }
    state.labels[i] = AccountChooserCopy(env, label, ACCOUNT_CHOOSER_MAX_LABEL_CHARS, &state.label_bytes[i]);
    if (label != NULL) (*env)->DeleteLocalRef(env, label);
    if (state.labels[i] == NULL) { AccountChooserFree(&state); return -1; }
  }
  if (!AccountChooserActive(&state) || (count == 0 && !state.can_add)) {
    AccountChooserFree(&state);
    return -1;
  }
  union { DWORD alignment; WORD words[128]; } buffer;
  ZeroMemory(&buffer, sizeof(buffer));
  DLGTEMPLATE* dialog = (DLGTEMPLATE*)buffer.words;
  dialog->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER | DS_SETFONT;
  dialog->cx = 300;
  dialog->cy = 210;
  WORD* at = (WORD*)(dialog + 1);
  *at++ = 0; /* No menu. */
  *at++ = 0; /* Predefined dialog class. */
  const WCHAR title[] = L"Gesher accounts";
  for (SIZE_T i = 0; i < sizeof(title) / sizeof(title[0]); ++i) *at++ = title[i];
  *at++ = 9;
  const WCHAR font[] = L"Segoe UI";
  for (SIZE_T i = 0; i < sizeof(font) / sizeof(font[0]); ++i) *at++ = font[i];
  /* A DialogBox failure can also return zero. Only an explicit dialog action
   * sets state.result, so a window-creation failure cannot select row zero. */
  DialogBoxIndirectParamW(GetModuleHandleW(NULL), dialog, NULL, AccountChooserDialog, (LPARAM)&state);
  jint result = state.result;
  AccountChooserFree(&state);
  return result;
}
