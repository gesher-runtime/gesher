/* Diagnostic branch only. Actual unmodified production WinRT boundary. */
#include "../compat/devsvc/devsvc_toast.c"

void gesher_toast_activated(const wchar_t* app_id, const wchar_t* tag,
    const wchar_t* group, const wchar_t* arguments) {
  (void)app_id; (void)tag; (void)group; (void)arguments;
  fputs("Unexpected external activation in isolated property diagnostic\n", stderr);
  ExitProcess(3);
}

static int Probe(const wchar_t* app_id, unsigned length) {
  wchar_t tag[66];
  const wchar_t alphabet[] = L"0123456789abcdef";
  for (unsigned i = 0; i < length; ++i) tag[i] = alphabet[i % 16];
  tag[length] = L'\0';
  const wchar_t* xml = L"<toast launch=\"diagnostic-owned-launch\"><visual>"
      L"<binding template=\"ToastGeneric\"><text>Gesher native identity audit</text>"
      L"</binding></visual></toast>";
  printf("TOAST_CASE length=%u\n", length); fflush(stdout);
  HRESULT hr = (HRESULT)gesher_toast_show(app_id, tag, L"android", xml, 1, 0);
  printf("TOAST_SHOW result=0x%08lx kept=%zu\n", (unsigned long)hr, g_count);
  int failed = FAILED(hr) || g_count != 1;
  if (!failed) {
    IToast2* toast2 = NULL;
    HRESULT query = QUERY(g_kept[0].toast,
        IID___x_ABI_CWindows_CUI_CNotifications_CIToastNotification2, &toast2);
    HSTRING actual_tag = NULL, actual_group = NULL;
    HRESULT tag_hr = SUCCEEDED(query) ? toast2->lpVtbl->get_Tag(toast2, &actual_tag) : query;
    HRESULT group_hr = SUCCEEDED(query) ? toast2->lpVtbl->get_Group(toast2, &actual_group) : query;
    int same = SUCCEEDED(tag_hr) && SUCCEEDED(group_hr)
        && StringEquals(actual_tag, tag) && StringEquals(actual_group, L"android");
    printf("TOAST_ROUNDTRIP length=%u getTag=0x%08lx getGroup=0x%08lx same=%d\n",
        length, (unsigned long)tag_hr, (unsigned long)group_hr, same);
    failed |= !same;
    HsFree(actual_tag); HsFree(actual_group); RELEASE(toast2);
  }
  Sleep(1500);
  TraceToastHistory("delayed-real-history", app_id, tag, L"android");
  HRESULT cleared = (HRESULT)gesher_toast_clear(app_id);
  printf("TOAST_CLEAR result=0x%08lx kept=%zu\n", (unsigned long)cleared, g_count);
  fflush(stdout);
  return failed || FAILED(cleared) || g_count != 0;
}

int main(void) {
  HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  if (FAILED(hr)) return 2;
  SetEnvironmentVariableW(L"GESHER_TRACE_TOAST_IDENTITY", L"1");
  const wchar_t* app_id = L"Gesher.Diagnostic.PR199.NativeIdentity";
  const wchar_t* key_name = L"Software\\Classes\\AppUserModelId\\Gesher.Diagnostic.PR199.NativeIdentity";
  HKEY key = NULL;
  DWORD disposition = 0;
  LONG status = RegCreateKeyExW(HKEY_CURRENT_USER, key_name, 0, NULL, 0,
      KEY_SET_VALUE, NULL, &key, &disposition);
  if (status != ERROR_SUCCESS || disposition != REG_CREATED_NEW_KEY) {
    if (key) RegCloseKey(key);
    fprintf(stderr, "Own AUMID registry creation failed: %ld/%lu\n", status, disposition);
    CoUninitialize(); return 2;
  }
  const wchar_t display[] = L"Gesher exact-source identity diagnostic";
  status = RegSetValueExW(key, L"DisplayName", 0, REG_SZ, (const BYTE*)display, sizeof(display));
  RegCloseKey(key);
  if (status != ERROR_SUCCESS) {
    RegDeleteTreeW(HKEY_CURRENT_USER, key_name); CoUninitialize(); return 2;
  }
  unsigned lengths[] = {64, 63, 32, 16};
  int failed = 0;
  for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
    failed += Probe(app_id, lengths[i]);
  }
  status = RegDeleteTreeW(HKEY_CURRENT_USER, key_name);
  CoUninitialize();
  printf("TOAST_AUDIT failed=%d cleanup=%ld\n", failed, status);
  return failed || status != ERROR_SUCCESS;
}
