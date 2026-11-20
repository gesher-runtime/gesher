/* Isolated diagnostic only. Executes the exact production WinRT boundary. */
#include "../compat/devsvc/devsvc_toast.c"

void gesher_toast_activated(const wchar_t* app_id, const wchar_t* tag,
    const wchar_t* group, const wchar_t* arguments) {
  (void)app_id; (void)tag; (void)group; (void)arguments;
  fputs("Unexpected external activation in isolated migration diagnostic\n", stderr);
  ExitProcess(3);
}

/* Independent RFC 4648 vectors: full 32 bytes, including the leading zero in
 * peer. Java codec tests independently decode every one-bit digest as well. */
static const wchar_t raw_tag[] = L"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static const wchar_t encoded_tag[] = L"sha256-ASNFZ4mrze8BI0VniavN7wEjRWeJq83vASNFZ4mrze8";
static const wchar_t peer_tag[] = L"sha256-ACNFZ4mrze8BI0VniavN7wEjRWeJq83vASNFZ4mrze8";
static const wchar_t legacy_tag[] = L"7|abcd";
static const wchar_t owner[] = L"n|migration-owner";
static const wchar_t peer_owner[] = L"n|migration-peer";
static const wchar_t group_owner[] = L"n|different-group";
static const wchar_t legacy_owner[] = L"n|legacy-other";

enum { RAW = 1, ENCODED = 2, PEER = 4, GROUP = 8, LEGACY = 16 };

static int Snapshot(const wchar_t* app_id, const char* phase, unsigned expected) {
  IToastHistory* history = NULL;
  IToastHistory2* history2 = NULL;
  IToastList* list = NULL;
  HSTRING id = NULL;
  HRESULT hr = History(&history);
  if (SUCCEEDED(hr)) hr = QUERY(history, IID___x_ABI_CWindows_CUI_CNotifications_CIToastNotificationHistory2, &history2);
  if (SUCCEEDED(hr)) hr = HsChecked(app_id, &id);
  if (SUCCEEDED(hr)) hr = history2->lpVtbl->GetHistoryWithId(history2, id, &list);
  UINT32 count = 0;
  if (SUCCEEDED(hr) && list == NULL) hr = E_UNEXPECTED;
  if (SUCCEEDED(hr)) hr = list->lpVtbl->get_Size(list, &count);
  unsigned mask = 0;
  int failed = FAILED(hr);
  for (UINT32 at = 0; SUCCEEDED(hr) && at < count; ++at) {
    IToast* toast = NULL;
    IToast2* toast2 = NULL;
    IXmlDoc* content = NULL;
    IXmlElement* root = NULL;
    IXmlSerializer* serializer = NULL;
    HSTRING tag = NULL, group = NULL, launch = NULL, name = NULL, xml = NULL;
    HRESULT item_hr = list->lpVtbl->GetAt(list, at, &toast);
    if (SUCCEEDED(item_hr)) item_hr = QUERY(toast, IID___x_ABI_CWindows_CUI_CNotifications_CIToastNotification2, &toast2);
    HRESULT tag_hr = SUCCEEDED(item_hr) ? toast2->lpVtbl->get_Tag(toast2, &tag) : item_hr;
    if (SUCCEEDED(item_hr)) item_hr = toast2->lpVtbl->get_Group(toast2, &group);
    if (SUCCEEDED(item_hr)) item_hr = toast->lpVtbl->get_Content(toast, &content);
    if (SUCCEEDED(item_hr)) item_hr = content->lpVtbl->get_DocumentElement(content, &root);
    if (SUCCEEDED(item_hr)) item_hr = HsChecked(L"launch", &name);
    if (SUCCEEDED(item_hr)) item_hr = root->lpVtbl->GetAttribute(root, name, &launch);
    if (SUCCEEDED(item_hr)) item_hr = QUERY(content, IID___x_ABI_CWindows_CData_CXml_CDom_CIXmlNodeSerializer, &serializer);
    if (SUCCEEDED(item_hr)) item_hr = serializer->lpVtbl->GetXml(serializer, &xml);
    unsigned role = 0;
    if (SUCCEEDED(item_hr)) {
      if (SUCCEEDED(tag_hr) && StringEquals(tag, encoded_tag)) {
        if (StringEquals(group, L"android") && StringEquals(launch, owner)) role = ENCODED;
        else if (StringEquals(group, L"other-owner-group") && StringEquals(launch, group_owner)) role = GROUP;
      } else if (SUCCEEDED(tag_hr) && StringEquals(tag, peer_tag)
          && StringEquals(group, L"android") && StringEquals(launch, peer_owner)) role = PEER;
      else if (SUCCEEDED(tag_hr) && StringEquals(tag, legacy_tag)
          && StringEquals(group, L"android") && StringEquals(launch, legacy_owner)) role = LEGACY;
      else if (((SUCCEEDED(tag_hr) && StringEquals(tag, raw_tag)) || tag_hr == E_INVALIDARG)
          && StringEquals(group, L"android") && StringEquals(launch, owner)) role = RAW;
      // Both old and new toasts carry the same complete activation payload.
      if (role == RAW || role == ENCODED) {
        UINT32 size = 0;
        const wchar_t* text = g_raw_buffer(xml, &size);
        if (text == NULL || wcsstr(text, L"arguments=\"a|0|migration-owner\"") == NULL) role = 0;
      }
    }
    printf("MIGRATION_ITEM phase=%s at=%u status=0x%08lx getTag=0x%08lx role=%u\n",
        phase, (unsigned)at, (unsigned long)item_hr, (unsigned long)tag_hr, role);
    TraceToastHString("tag", tag); TraceToastHString("group", group);
    TraceToastHString("launch", launch); TraceToastHString("xml", xml); fputc('\n', stderr);
    if (role == 0 || (mask & role) != 0) failed = 1;
    mask |= role;
    HsFree(xml); HsFree(name); HsFree(launch); HsFree(group); HsFree(tag);
    RELEASE(serializer); RELEASE(root); RELEASE(content); RELEASE(toast2); RELEASE(toast);
  }
  if (mask != expected) failed = 1;
  printf("MIGRATION_SNAPSHOT phase=%s status=0x%08lx count=%u expected=%u actual=%u failed=%d kept=%zu\n",
      phase, (unsigned long)hr, (unsigned)count, expected, mask, failed, g_count);
  HsFree(id); RELEASE(list); RELEASE(history2); RELEASE(history);
  fflush(stdout); fflush(stderr);
  return failed;
}

static int Post(const wchar_t* app_id, const wchar_t* tag,
    const wchar_t* group, const wchar_t* launch, int action) {
  wchar_t xml[1024];
  int length = swprintf(xml, sizeof(xml) / sizeof(xml[0]),
      L"<toast launch=\"%ls\"><visual><binding template=\"ToastGeneric\">"
      L"<text>Gesher native migration audit</text></binding></visual>%ls</toast>",
      launch, action ? L"<actions><action content=\"Open\" arguments=\"a|0|migration-owner\"/></actions>" : L"");
  if (length < 0 || (size_t)length >= sizeof(xml) / sizeof(xml[0])) return 1;
  HRESULT hr = (HRESULT)gesher_toast_show(app_id, tag, group, xml, 1, 0);
  printf("MIGRATION_SHOW status=0x%08lx length=%zu kept=%zu\n", (unsigned long)hr, wcslen(tag), g_count);
  return FAILED(hr);
}

static int Remove(const wchar_t* app_id, const wchar_t* tag,
    const wchar_t* group, const wchar_t* launch, HRESULT expected, const char* phase) {
  HRESULT hr = (HRESULT)(launch == NULL ? gesher_toast_remove(app_id, tag, group)
      : gesher_toast_remove_if_launch(app_id, tag, group, launch));
  printf("MIGRATION_REMOVE phase=%s result=0x%08lx expected=0x%08lx kept=%zu\n",
      phase, (unsigned long)hr, (unsigned long)expected, g_count);
  fflush(stdout);
  return hr != expected;
}

int main(void) {
  HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  if (FAILED(hr)) return 2;
  SetEnvironmentVariableW(L"GESHER_TRACE_TOAST_IDENTITY", L"1");
  const wchar_t* app_id = L"Gesher.Diagnostic.PR199.NativeMigration";
  const wchar_t* key_name = L"Software\\Classes\\AppUserModelId\\Gesher.Diagnostic.PR199.NativeMigration";
  HKEY key = NULL;
  DWORD disposition = 0;
  LONG status = RegCreateKeyExW(HKEY_CURRENT_USER, key_name, 0, NULL, 0,
      KEY_SET_VALUE, NULL, &key, &disposition);
  if (status != ERROR_SUCCESS || disposition != REG_CREATED_NEW_KEY) {
    if (key) RegCloseKey(key);
    fprintf(stderr, "Own AUMID registry creation failed: %ld/%lu\n", status, disposition);
    CoUninitialize(); return 2;
  }
  const wchar_t display[] = L"Gesher exact-source migration diagnostic";
  status = RegSetValueExW(key, L"DisplayName", 0, REG_SZ, (const BYTE*)display, sizeof(display));
  RegCloseKey(key);
  if (status != ERROR_SUCCESS) {
    RegDeleteTreeW(HKEY_CURRENT_USER, key_name); CoUninitialize(); return 2;
  }
  int failed = 0;
  failed += Post(app_id, raw_tag, L"android", owner, 1);
  failed += Post(app_id, encoded_tag, L"android", owner, 1);
  failed += Post(app_id, peer_tag, L"android", peer_owner, 0);
  failed += Post(app_id, encoded_tag, L"other-owner-group", group_owner, 0);
  failed += Post(app_id, legacy_tag, L"android", legacy_owner, 0);
  Sleep(1500);
  failed += Snapshot(app_id, "seeded", RAW | ENCODED | PEER | GROUP | LEGACY);

  // Drop process references without Hide or Clear. The real history call must
  // remove the old raw identity; no live popup handle can mask a broken remove.
  IToast* kept[8];
  size_t taken = TakeKept(L"Gesher.Diagnostic.PR199.NativeMigration\n", 0, kept, 8);
  for (size_t i = 0; i < taken; ++i) RELEASE(kept[i]);
  printf("MIGRATION_RELEASE taken=%zu kept=%zu hide=0 clear=0\n", taken, g_count);
  failed += taken != 5 || g_count != 0;
  Sleep(500);
  failed += Snapshot(app_id, "released", RAW | ENCODED | PEER | GROUP | LEGACY);
  failed += Remove(app_id, raw_tag, L"android", NULL, S_OK, "raw64-per-tag");
  Sleep(500);
  failed += Snapshot(app_id, "raw64-retired", ENCODED | PEER | GROUP | LEGACY);
  failed += Remove(app_id, raw_tag, L"android", NULL, S_OK, "raw64-idempotent");
  failed += Remove(app_id, encoded_tag, L"missing-group", NULL, S_OK, "wrong-group");
  failed += Remove(app_id, encoded_tag, L"android", peer_owner, S_FALSE, "wrong-owner");
  failed += Snapshot(app_id, "negative-controls", ENCODED | PEER | GROUP | LEGACY);
  failed += Remove(app_id, encoded_tag, L"android", owner, S_OK, "encoded-owned");
  Sleep(500);
  failed += Snapshot(app_id, "encoded-retired", PEER | GROUP | LEGACY);
  failed += Remove(app_id, legacy_tag, L"android", owner, S_FALSE, "legacy-wrong-owner");
  failed += Snapshot(app_id, "legacy-protected", PEER | GROUP | LEGACY);
  failed += Remove(app_id, legacy_tag, L"android", legacy_owner, S_OK, "legacy-owned");
  failed += Remove(app_id, peer_tag, L"android", NULL, S_OK, "peer-owned");
  Sleep(500);
  failed += Snapshot(app_id, "group-protected", GROUP);
  failed += Remove(app_id, encoded_tag, L"other-owner-group", NULL, S_OK, "group-owned");
  Sleep(500);
  failed += Snapshot(app_id, "per-tag-empty", 0);
  // Cleanup of our isolated AUMID only, after all per-tag assertions above.
  HRESULT cleanup = (HRESULT)gesher_toast_clear(app_id);
  status = RegDeleteTreeW(HKEY_CURRENT_USER, key_name);
  CoUninitialize();
  printf("TOAST_MIGRATION_AUDIT failed=%d cleanup=0x%08lx registry=%ld kept=%zu\n",
      failed, (unsigned long)cleanup, status, g_count);
  return failed || FAILED(cleanup) || status != ERROR_SUCCESS || g_count != 0;
}
