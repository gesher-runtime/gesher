/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Included by the trusted account library and native UI contract test. */
static BOOL DeviceAdminDialog(enum GesherUiLanguage language, const WCHAR* label,
        const WCHAR* description, BOOL supported) {
  SIZE_T label_length = wcslen(label), description_length = wcslen(description);
  const WCHAR* policy = GesherUiText(language, supported ? "device_admin_lock" : "device_admin_unsupported");
  const WCHAR* app_info = GesherUiText(language, "device_admin_app_explanation");
  SIZE_T capacity = (SIZE_T)label_length + description_length + wcslen(policy) + wcslen(app_info) + 20;
  WCHAR* message = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, capacity * sizeof(WCHAR));
  if (message == NULL) return FALSE;
  SIZE_T at = 0;
  /* Use classic embedding marks supported by Windows' native text controls. */
  message[at++] = 0x202a;
  memcpy(message + at, label, label_length * sizeof(WCHAR)); at += label_length;
  message[at++] = 0x202c; message[at++] = L'\n'; message[at++] = L'\n';
  wcscpy(message + at, policy); at += wcslen(policy);
  if (description_length > 0) {
    message[at++] = L'\n'; message[at++] = L'\n';
    wcscpy(message + at, app_info); at += wcslen(app_info);
    message[at++] = L'\n';
    memcpy(message + at, description, description_length * sizeof(WCHAR));
  }
  BOOL accepted = FALSE;
  accepted = GesherHostPrompt(
      GesherUiText(language, "device_admin_title"), message,
      GesherUiText(language, supported ? "device_admin_activate" : "cp_dialog_ok"),
      supported ? GesherUiText(language, "device_admin_cancel") : NULL,
      GesherUiRtl(language), NULL, MB_ICONWARNING);
  SecureZeroMemory(message, capacity * sizeof(WCHAR)); HeapFree(GetProcessHeap(), 0, message);
  return supported && accepted;
}
