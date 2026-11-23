/* Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0 */
/* Included by account_crypto.c: shares its existing system-server role guard. */

enum {
  ACCOUNT_CHOOSER_MAX_LABELS = 256,
  ACCOUNT_CHOOSER_MAX_LABEL_CHARS = 2048,
  ACCOUNT_CHOOSER_MAX_PACKAGE_CHARS = 1024,
  ACCOUNT_CHOOSER_LIST = 1001,
  ACCOUNT_CHOOSER_ADD = 1002,
  ACCOUNT_CHOOSER_BRAND = 1003,
  ACCOUNT_CHOOSER_PROMPT = 1004,
  ACCOUNT_CHOOSER_TIMER = 1
};

typedef struct {
  JNIEnv* env;
  jobject is_active;
  jmethodID active_method;
  DWORD thread;
  enum GesherUiLanguage language;
  BOOL can_add;
  BOOL ended;
  HFONT heading_font;
  HFONT brand_font;
  GesherHostTheme theme;
  jsize count;
  jint result;
  WCHAR* prompt;
  SIZE_T prompt_bytes;
  BOOL selectable[ACCOUNT_CHOOSER_MAX_LABELS];
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
  if (state->prompt != NULL) {
    SecureZeroMemory(state->prompt, state->prompt_bytes);
    HeapFree(GetProcessHeap(), 0, state->prompt);
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
    GesherHostThemeInit(&state->theme, dialog);
    BOOL rtl = GesherUiRtl(state->language);
    DWORD reading = rtl ? WS_EX_RTLREADING : 0;
    DWORD alignment = rtl ? SS_RIGHT : SS_LEFT;
    HWND brand = AccountChooserControl(dialog, reading, L"STATIC",
        GesherUiText(state->language, "brand"), alignment,
        ACCOUNT_CHOOSER_BRAND, 16, 10, 288, 14);
    HWND prompt = AccountChooserControl(dialog, reading, L"STATIC",
        state->prompt != NULL ? state->prompt : GesherUiText(state->language, "chooser_prompt"),
        // Wraps to the two lines the heading font fits; ellipsis only after that.
        alignment | SS_EDITCONTROL | SS_ENDELLIPSIS | SS_NOPREFIX, ACCOUNT_CHOOSER_PROMPT,
        16, 30, 288, 26);
    // A package identifier remains LTR even when the host UI uses Hebrew.
    HWND package = AccountChooserControl(dialog, WS_EX_LTRREADING, L"STATIC",
        state->package_name, SS_LEFT | SS_ENDELLIPSIS, -1, 16, 58, 288, 18);
    HWND list = AccountChooserControl(dialog, reading, L"LISTBOX", L"",
        WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        ACCOUNT_CHOOSER_LIST, 16, 82, 288, 126);
    HWND select = AccountChooserControl(dialog, 0, L"BUTTON",
        GesherUiText(state->language, "chooser_select"),
        WS_TABSTOP | BS_OWNERDRAW, IDOK, 140, 218, 80, 22);
    HWND cancel = AccountChooserControl(dialog, 0, L"BUTTON",
        GesherUiText(state->language, "chooser_cancel"),
        WS_TABSTOP | BS_OWNERDRAW, IDCANCEL, 228, 218, 76, 22);
    HWND add = state->can_add ? AccountChooserControl(dialog, 0, L"BUTTON",
        GesherUiText(state->language, "chooser_add"),
        WS_TABSTOP | BS_OWNERDRAW, ACCOUNT_CHOOSER_ADD, 16, 218, 114, 22) : NULL;
    if (!brand || !prompt || !package || !list || !select || !cancel
        || (state->can_add && !add)) {
      AccountChooserFinish(dialog, state, -1);
      return FALSE;
    }
    GesherHostSetFont(prompt, &state->heading_font, 145, FW_SEMIBOLD);
    GesherHostSetFont(brand, &state->brand_font, 100, FW_BOLD);
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
  if (message == GESHER_HOST_REFONT) {
    GesherHostSetFont(GetDlgItem(dialog, ACCOUNT_CHOOSER_PROMPT), &state->heading_font,
                      145, FW_SEMIBOLD);
    GesherHostSetFont(GetDlgItem(dialog, ACCOUNT_CHOOSER_BRAND), &state->brand_font,
                      100, FW_BOLD);
    return TRUE;
  }
  if (message == WM_CLOSE) {
    AccountChooserFinish(dialog, state, -1);
    return TRUE;
  }
  if (message == WM_DESTROY) {
    KillTimer(dialog, ACCOUNT_CHOOSER_TIMER);
    if (state->heading_font != NULL) DeleteObject(state->heading_font);
    if (state->brand_font != NULL) DeleteObject(state->brand_font);
    GesherHostThemeDispose(&state->theme);
    SetWindowLongPtrW(dialog, DWLP_USER, 0);
    return TRUE;
  }
  if (message != WM_COMMAND)
    return GesherHostThemeEvent(dialog, &state->theme, message, wparam, lparam,
                                ACCOUNT_CHOOSER_BRAND);
  int command = LOWORD(wparam);
  if (command == IDCANCEL) {
    AccountChooserFinish(dialog, state, -1);
    return TRUE;
  }
  if (command == ACCOUNT_CHOOSER_LIST && HIWORD(wparam) == LBN_SELCHANGE) {
    if (!AccountChooserActive(state)) { AccountChooserFinish(dialog, state, -1); return TRUE; }
    LRESULT selected = SendDlgItemMessageW(dialog, ACCOUNT_CHOOSER_LIST, LB_GETCURSEL, 0, 0);
    EnableWindow(GetDlgItem(dialog, IDOK), selected >= 0 && selected < state->count && state->selectable[selected]);
    return TRUE;
  }
  if (command == IDOK || command == ACCOUNT_CHOOSER_ADD) {
    if (!AccountChooserActive(state)) { AccountChooserFinish(dialog, state, -1); return TRUE; }
    if (command == ACCOUNT_CHOOSER_ADD) {
      if (state->can_add) AccountChooserFinish(dialog, state, -2);
    } else {
      if (!IsWindowEnabled(GetDlgItem(dialog, IDOK))) return TRUE;
      LRESULT selected = SendDlgItemMessageW(dialog, ACCOUNT_CHOOSER_LIST, LB_GETCURSEL, 0, 0);
      if (selected >= 0 && selected < state->count && state->selectable[selected]) AccountChooserFinish(dialog, state, (jint)selected);
    }
    return TRUE;
  }
  return FALSE;
}

static jint ChoiceDialog(JNIEnv* env, jstring package_name, jobjectArray labels,
        jboolean can_add, jobject is_active, jstring prompt, BOOL intent_choice, jbooleanArray enabled) {
  if (!RequireServer(env)) return -1;
  if (package_name == NULL || labels == NULL || is_active == NULL) {
    Fail(env, "java/lang/IllegalArgumentException", "Missing account chooser input");
    return -1;
  }
  jsize count = (*env)->GetArrayLength(env, labels);
  if (count < 0 || count > (intent_choice ? ACCOUNT_CHOOSER_MAX_LABELS : 64)) {
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
  state.language = GesherUiCurrentLanguage();
  state.env = env;
  state.is_active = is_active;
  state.active_method = active_method;
  state.thread = GetCurrentThreadId();
  state.count = count;
  state.can_add = can_add == JNI_TRUE;
  state.result = intent_choice ? -3 : -1;
  if (enabled != NULL && (*env)->GetArrayLength(env, enabled) != count) {
    Fail(env, "java/lang/IllegalArgumentException", "Mismatched intent chooser enabled rows");
    return -3;
  }
  jboolean enabled_values[ACCOUNT_CHOOSER_MAX_LABELS];
  if (enabled != NULL) {
    (*env)->GetBooleanArrayRegion(env, enabled, 0, count, enabled_values);
    if ((*env)->ExceptionCheck(env)) return -3;
  }
  for (jsize i = 0; i < count; ++i) state.selectable[i] = enabled == NULL || enabled_values[i] == JNI_TRUE;
  if (prompt != NULL) {
    state.prompt = AccountChooserCopy(env, prompt, ACCOUNT_CHOOSER_MAX_LABEL_CHARS, &state.prompt_bytes);
    if (state.prompt == NULL) { AccountChooserFree(&state); return -3; }
  }
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
  dialog->dwExtendedStyle = GesherUiRtl(state.language) ? WS_EX_RTLREADING : 0;
  dialog->cx = 320;
  dialog->cy = 248;
  WORD* at = (WORD*)(dialog + 1);
  *at++ = 0; /* No menu. */
  *at++ = 0; /* Predefined dialog class. */
  const WCHAR* title = GesherUiText(state.language, intent_choice ? "intent_chooser_title" : "chooser_title");
  SIZE_T title_chars = wcslen(title) + 1;
  const WCHAR font[] = L"Segoe UI";
  if ((SIZE_T)(at - buffer.words) + title_chars + 1 + sizeof(font) / sizeof(font[0])
      > sizeof(buffer.words) / sizeof(buffer.words[0])) {
    AccountChooserFree(&state);
    return -1;
  }
  for (SIZE_T i = 0; i < title_chars; ++i) *at++ = title[i];
  *at++ = 9;
  for (SIZE_T i = 0; i < sizeof(font) / sizeof(font[0]); ++i) *at++ = font[i];
  /* A DialogBox failure can also return zero. Only an explicit dialog action
   * sets state.result, so a window-creation failure cannot select row zero. */
  DialogBoxIndirectParamW(GetModuleHandleW(NULL), dialog, NULL, AccountChooserDialog, (LPARAM)&state);
  jint result = state.result;
  AccountChooserFree(&state);
  return result;
}


JNIEXPORT jint GESHER_NATIVE
Java_com_gesher_server_AccountChooser_nativeChoose(JNIEnv* env, jclass cls,
        jstring package_name, jobjectArray labels, jboolean can_add, jobject is_active) {
  (void)cls;
  return ChoiceDialog(env, package_name, labels, can_add, is_active, NULL, FALSE, NULL);
}

JNIEXPORT jboolean GESHER_NATIVE
Java_com_gesher_server_IntentChooser_nativeAvailable(JNIEnv* env, jclass cls) {
  (void)cls;
  if (!RequireServer(env)) return JNI_FALSE;
  USEROBJECTFLAGS flags = {0};
  DWORD bytes = 0;
  HWINSTA station = GetProcessWindowStation();
  return station != NULL && GetUserObjectInformationW(station, UOI_FLAGS, &flags,
      sizeof(flags), &bytes) && (flags.dwFlags & WSF_VISIBLE) != 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint GESHER_NATIVE
Java_com_gesher_server_IntentChooser_nativeChoose(JNIEnv* env, jclass cls,
        jstring requester, jstring title, jobjectArray labels, jbooleanArray enabled, jobject is_active) {
  (void)cls;
  if (title == NULL || enabled == NULL) {
    Fail(env, "java/lang/IllegalArgumentException", "Missing intent chooser title");
    return -3;
  }
  return ChoiceDialog(env, requester, labels, JNI_FALSE, is_active, title, TRUE, enabled);
}
