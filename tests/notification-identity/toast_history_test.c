/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
/* Actual production owner-check/removal code with explicit COM history/XML and
 * removal boundaries. Never queries or changes the user's live toast history. */
#include "../../compat/devsvc/devsvc_toast.c"
#include <stdio.h>

void gesher_toast_activated(const wchar_t* app, const wchar_t* tag, const wchar_t* group, const wchar_t* args) { }
static int mode, refs, removed, checks, index_in_list;
extern int toast_identity_argument_checks(void);
static int string_call, fail_string_call, unexpected_factory_calls;
static CreateStringFn real_create_string;
static HRESULT WINAPI CreateWithFailure(PCNZWCH chars, UINT32 length, HSTRING* out) {
  if (++string_call == fail_string_call) { *out = NULL; return E_OUTOFMEMORY; }
  return real_create_string(chars, length, out);
}
static HRESULT WINAPI UnexpectedFactory(HSTRING name, REFIID iid, void** out) {
  ++unexpected_factory_calls; *out = NULL; return E_FAIL;
}
static IToastHistory2 history2;
static IToastList list;
static IToast toast;
static IToast2 toast2;
static IXmlDoc doc;
static IXmlElement element;
static ULONG STDMETHODCALLTYPE Add(void* self) { return (ULONG)++refs; }
static ULONG STDMETHODCALLTYPE Drop(void* self) { --refs; return 1; }
static HRESULT STDMETHODCALLTYPE Query(void* self, REFIID iid, void** out) {
  *out = NULL;
  if (IsEqualGUID(iid, &IID___x_ABI_CWindows_CUI_CNotifications_CIToastNotificationHistory2)) {
    if (mode == 1) return E_NOINTERFACE;
    *out = &history2;
  } else {
    if (mode == 5) return E_NOINTERFACE;
    *out = &toast2;
  }
  Add(*out); return S_OK;
}
static HRESULT STDMETHODCALLTYPE HistoryList(IToastHistory2* self, HSTRING app, IToastList** out) {
  if (mode == 2) return E_FAIL;
  *out = &list; Add(*out); return S_OK;
}
static HRESULT STDMETHODCALLTYPE Size(IToastList* self, UINT32* out) {
  if (mode == 3) return E_FAIL;
  *out = mode == 20 ? 0 : mode == 21 ? 2 : 1; return S_OK;
}
static HRESULT STDMETHODCALLTYPE At(IToastList* self, UINT32 at, IToast** out) {
  if (mode == 4) return E_FAIL;
  index_in_list = at; *out = &toast; Add(*out); return S_OK;
}
static HRESULT STDMETHODCALLTYPE Tag(IToast2* self, HSTRING* out) {
  if (mode == 6) return E_FAIL;
  *out = Hs(mode == 22 || (mode == 21 && index_in_list == 0) ? L"other-tag" : L"7|hash"); return S_OK;
}
static HRESULT STDMETHODCALLTYPE Group(IToast2* self, HSTRING* out) {
  if (mode == 7) return E_FAIL;
  *out = Hs(mode == 23 ? L"other-group" : L"android"); return S_OK;
}
static HRESULT STDMETHODCALLTYPE Content(IToast* self, IXmlDoc** out) {
  if (mode == 8) return E_FAIL;
  *out = &doc; Add(*out); return S_OK;
}
static HRESULT STDMETHODCALLTYPE Root(IXmlDoc* self, IXmlElement** out) {
  if (mode == 9) return E_FAIL;
  *out = &element; Add(*out); return S_OK;
}
static HRESULT STDMETHODCALLTYPE Attribute(IXmlElement* self, HSTRING name, HSTRING* out) {
  if (!StringEquals(name, L"launch")) return E_INVALIDARG;
  if (mode == 10) return E_FAIL;
  *out = Hs(mode == 24 ? L"n|another-owner" : L"n|0|com.example|7|Aa&<>|10000"); return S_OK;
}
static long Remove(const wchar_t* app, const wchar_t* tag, const wchar_t* group) {
  ++removed; return mode == 25 ? E_ACCESSDENIED : S_OK;
}
#define FN(table, member, value) (__typeof__((table).member))(value)
static __x_ABI_CWindows_CUI_CNotifications_CIToastNotificationHistoryVtbl history_vtbl;
static __x_ABI_CWindows_CUI_CNotifications_CIToastNotificationHistory2Vtbl history2_vtbl;
static __FIVectorView_1_Windows__CUI__CNotifications__CToastNotificationVtbl list_vtbl;
static __x_ABI_CWindows_CUI_CNotifications_CIToastNotificationVtbl toast_vtbl;
static __x_ABI_CWindows_CUI_CNotifications_CIToastNotification2Vtbl toast2_vtbl;
static __x_ABI_CWindows_CData_CXml_CDom_CIXmlDocumentVtbl doc_vtbl;
static __x_ABI_CWindows_CData_CXml_CDom_CIXmlElementVtbl element_vtbl;
#define BASE(table) do { (table).QueryInterface = FN(table, QueryInterface, Query); \
  (table).AddRef = FN(table, AddRef, Add); (table).Release = FN(table, Release, Drop); } while (0)
static void Check(int ok, const char* why) { ++checks; if (!ok) { fprintf(stderr, "%s mode=%d\n", why, mode); exit(1); } }
int main(void) {
  checks += toast_identity_argument_checks();
  Check(RtOk(), "HSTRING runtime unavailable");
  BASE(history_vtbl); BASE(history2_vtbl); BASE(list_vtbl); BASE(toast_vtbl);
  BASE(toast2_vtbl); BASE(doc_vtbl); BASE(element_vtbl);
  history2_vtbl.GetHistoryWithId = HistoryList; list_vtbl.get_Size = Size; list_vtbl.GetAt = At;
  toast2_vtbl.get_Tag = Tag; toast2_vtbl.get_Group = Group; toast_vtbl.get_Content = Content;
  doc_vtbl.get_DocumentElement = Root; element_vtbl.GetAttribute = Attribute;
  IToastHistory history = {&history_vtbl}; history2.lpVtbl = &history2_vtbl; list.lpVtbl = &list_vtbl;
  toast.lpVtbl = &toast_vtbl; toast2.lpVtbl = &toast2_vtbl; doc.lpVtbl = &doc_vtbl; element.lpVtbl = &element_vtbl;
  int cases[] = {0,1,2,3,4,5,6,7,8,9,10,20,21,22,23,24,25};
  for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
    mode = cases[i]; refs = removed = 0;
    long result = RemoveHistoryChecked(&history, L"Gesher.App.fixture", L"7|hash", L"android",
                                       L"n|0|com.example|7|Aa&<>|10000", Remove);
    Check(refs == 0, "COM reference leaked");
    if (mode == 0 || mode == 21) Check(result == S_OK && removed == 1, "matching owner not removed");
    else if (mode == 20 || mode == 22 || mode == 23) Check(result == S_OK && removed == 0, "absent identity changed history");
    else if (mode == 24) Check(result == S_FALSE && removed == 0, "colliding owner was removed");
    else if (mode == 25) Check(result == E_ACCESSDENIED && removed == 1, "removal failure fabricated success");
    else Check(FAILED(result) && removed == 0, "history/XML failure removed a toast");
  }
  /* Exercise the real final remove function's allocation failures, before any
   * history lookup or mutation. A NULL HSTRING must not become an empty identity. */
  real_create_string = g_create_string;
  GetFactoryFn factory = g_get_factory;
  g_create_string = CreateWithFailure; g_get_factory = UnexpectedFactory;
  for (fail_string_call = 1; fail_string_call <= 3; ++fail_string_call) {
    string_call = unexpected_factory_calls = 0;
    Check(RemoveUnlocked(L"Gesher.App.fixture", L"7|hash", L"android") == E_OUTOFMEMORY,
          "HSTRING allocation failure did not propagate");
    Check(unexpected_factory_calls == 0, "failed identity allocation reached live history");
  }
  g_create_string = real_create_string; g_get_factory = factory;
  printf("Toast history identity checks passed: %d checks\n", checks);
  return 0;
}
