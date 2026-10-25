/* Gesher: Windows toasts for the notification service (devsvc track); see
 * devsvc_toast.h. WinRT is reached through combase's functions, looked up when
 * first used; no WinRT library is linked. */

#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>

#include <initguid.h> /* the interface ids of the headers below, defined here */
#include <inspectable.h>
#include <winstring.h>
#include <windows.data.xml.dom.h>
#include <windows.ui.notifications.h>

#include <stdlib.h>
#include <string.h>

#include "devsvc_toast.h"

typedef __x_ABI_CWindows_CUI_CNotifications_CIToastNotification IToast;
typedef __x_ABI_CWindows_CUI_CNotifications_CIToastNotification2 IToast2;
typedef __x_ABI_CWindows_CUI_CNotifications_CIToastNotification4 IToast4;
typedef __x_ABI_CWindows_CUI_CNotifications_CIToastNotificationFactory IToastFactory;
typedef __x_ABI_CWindows_CUI_CNotifications_CIToastNotificationManagerStatics IToastStatics;
typedef __x_ABI_CWindows_CUI_CNotifications_CIToastNotificationManagerStatics2 IToastStatics2;
typedef __x_ABI_CWindows_CUI_CNotifications_CIToastNotificationHistory IToastHistory;
typedef __x_ABI_CWindows_CUI_CNotifications_CIToastNotifier IToastNotifier;
typedef __x_ABI_CWindows_CUI_CNotifications_CIToastActivatedEventArgs IToastActivatedArgs;
typedef __x_ABI_CWindows_CData_CXml_CDom_CIXmlDocument IXmlDoc;
typedef __x_ABI_CWindows_CData_CXml_CDom_CIXmlDocumentIO IXmlDocIO;
typedef __FITypedEventHandler_2_Windows__CUI__CNotifications__CToastNotification_IInspectable
    ActivatedHandler;
typedef __FITypedEventHandler_2_Windows__CUI__CNotifications__CToastNotification_IInspectableVtbl
    ActivatedHandlerVtbl;

/* --- combase ---------------------------------------------------------------------- */

typedef HRESULT(WINAPI* GetFactoryFn)(HSTRING, REFIID, void**);
typedef HRESULT(WINAPI* ActivateFn)(HSTRING, IInspectable**);
typedef HRESULT(WINAPI* CreateStringFn)(PCNZWCH, UINT32, HSTRING*);
typedef HRESULT(WINAPI* DeleteStringFn)(HSTRING);
typedef PCWSTR(WINAPI* RawBufferFn)(HSTRING, UINT32*);

static GetFactoryFn g_get_factory;
static ActivateFn g_activate;
static CreateStringFn g_create_string;
static DeleteStringFn g_delete_string;
static RawBufferFn g_raw_buffer;
static INIT_ONCE g_rt_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK LoadCombase(PINIT_ONCE once, void* param, void** context) {
  (void)once;
  (void)param;
  (void)context;
  HMODULE m = LoadLibraryW(L"combase.dll");
  if (m == NULL) return TRUE;
  g_get_factory = (GetFactoryFn)(void*)GetProcAddress(m, "RoGetActivationFactory");
  g_activate = (ActivateFn)(void*)GetProcAddress(m, "RoActivateInstance");
  g_create_string = (CreateStringFn)(void*)GetProcAddress(m, "WindowsCreateString");
  g_delete_string = (DeleteStringFn)(void*)GetProcAddress(m, "WindowsDeleteString");
  g_raw_buffer = (RawBufferFn)(void*)GetProcAddress(m, "WindowsGetStringRawBuffer");
  return TRUE;
}

static int RtOk(void) {
  InitOnceExecuteOnce(&g_rt_once, LoadCombase, NULL, NULL);
  return g_get_factory && g_activate && g_create_string && g_delete_string && g_raw_buffer;
}

static HSTRING Hs(const wchar_t* s) {
  HSTRING h = NULL;
  g_create_string(s, (UINT32)wcslen(s), &h);
  return h;
}

static void HsFree(HSTRING h) {
  if (h != NULL) g_delete_string(h);
}

/* A copy of `h` (NUL-terminated, malloc'd), which is deleted. */
static wchar_t* HsTake(HSTRING h) {
  UINT32 n = 0;
  PCWSTR p = h != NULL ? g_raw_buffer(h, &n) : NULL;
  wchar_t* out = (wchar_t*)malloc((n + 1) * sizeof(wchar_t));
  if (out != NULL) {
    if (n > 0) memcpy(out, p, n * sizeof(wchar_t));
    out[n] = L'\0';
  }
  HsFree(h);
  return out;
}

#define RELEASE(p)                                \
  do {                                            \
    if ((p) != NULL) {                            \
      IUnknown* u_ = (IUnknown*)(p);              \
      u_->lpVtbl->Release(u_);                    \
      (p) = NULL;                                 \
    }                                             \
  } while (0)

#define QUERY(p, iid, out) (((IUnknown*)(p))->lpVtbl->QueryInterface((IUnknown*)(p), &(iid), (void**)(out)))

/* --- the Activated handler -------------------------------------------------------- */

typedef struct {
  ActivatedHandler base; /* first: the interface pointer is the object's address */
  volatile LONG refs;
  wchar_t* app_id;
} OnActivated;

static HRESULT STDMETHODCALLTYPE ActivatedQuery(ActivatedHandler* self, REFIID iid, void** out) {
  if (out == NULL) return E_POINTER;
  /* IAgileObject has no methods of its own: the handler may be called on any
   * thread (WinRT's thread pool calls it). */
  if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IAgileObject) ||
      IsEqualIID(iid,
                 &IID___FITypedEventHandler_2_Windows__CUI__CNotifications__CToastNotification_IInspectable)) {
    *out = self;
    InterlockedIncrement(&((OnActivated*)self)->refs);
    return S_OK;
  }
  *out = NULL;
  return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE ActivatedAddRef(ActivatedHandler* self) {
  return (ULONG)InterlockedIncrement(&((OnActivated*)self)->refs);
}

static ULONG STDMETHODCALLTYPE ActivatedRelease(ActivatedHandler* self) {
  OnActivated* o = (OnActivated*)self;
  LONG n = InterlockedDecrement(&o->refs);
  if (n == 0) {
    free(o->app_id);
    free(o);
  }
  return (ULONG)n;
}

static HRESULT STDMETHODCALLTYPE ActivatedInvoke(ActivatedHandler* self, IToast* sender,
                                                 IInspectable* args) {
  wchar_t* tag = NULL;
  wchar_t* group = NULL;
  wchar_t* arguments = NULL;
  IToast2* t2 = NULL;
  IToastActivatedArgs* e = NULL;
  if (sender != NULL &&
      SUCCEEDED(QUERY(sender, IID___x_ABI_CWindows_CUI_CNotifications_CIToastNotification2, &t2))) {
    HSTRING h = NULL;
    if (SUCCEEDED(t2->lpVtbl->get_Tag(t2, &h))) tag = HsTake(h);
    h = NULL;
    if (SUCCEEDED(t2->lpVtbl->get_Group(t2, &h))) group = HsTake(h);
    RELEASE(t2);
  }
  if (args != NULL &&
      SUCCEEDED(QUERY(args, IID___x_ABI_CWindows_CUI_CNotifications_CIToastActivatedEventArgs,
                      &e))) {
    HSTRING h = NULL;
    if (SUCCEEDED(e->lpVtbl->get_Arguments(e, &h))) arguments = HsTake(h);
    RELEASE(e);
  }
  gesher_toast_activated(((OnActivated*)self)->app_id, tag ? tag : L"", group ? group : L"",
                         arguments ? arguments : L"");
  free(tag);
  free(group);
  free(arguments);
  return S_OK;
}

static ActivatedHandlerVtbl g_activated_vtbl = {ActivatedQuery, ActivatedAddRef,
                                                ActivatedRelease, ActivatedInvoke};

static ActivatedHandler* NewActivatedHandler(const wchar_t* app_id) {
  OnActivated* o = (OnActivated*)calloc(1, sizeof(OnActivated));
  if (o == NULL) return NULL;
  o->base.lpVtbl = &g_activated_vtbl;
  o->refs = 1;
  o->app_id = _wcsdup(app_id);
  return &o->base;
}

/* --- the toasts shown and not removed --------------------------------------------- */
/* Kept so that their Activated handlers stay registered and Hide can take them
 * down; keyed by app id, group and tag. */

typedef struct {
  wchar_t* key;
  IToast* toast;
} Kept;

static SRWLOCK g_kept_lock = SRWLOCK_INIT;
static Kept* g_kept;   /* guarded by g_kept_lock */
static size_t g_count; /* guarded by g_kept_lock */
static size_t g_capacity;

static wchar_t* Key(const wchar_t* app_id, const wchar_t* tag, const wchar_t* group) {
  size_t n = wcslen(app_id) + wcslen(tag) + wcslen(group) + 3;
  wchar_t* k = (wchar_t*)malloc(n * sizeof(wchar_t));
  if (k != NULL) {
    wcscpy(k, app_id);
    wcscat(k, L"\n");
    wcscat(k, group);
    wcscat(k, L"\n");
    wcscat(k, tag);
  }
  return k;
}

/* Keeps `toast` under `key` (taken); returns the toast it replaces, if any. */
static IToast* Keep(wchar_t* key, IToast* toast) {
  IToast* old = NULL;
  AcquireSRWLockExclusive(&g_kept_lock);
  for (size_t i = 0; i < g_count; i++) {
    if (wcscmp(g_kept[i].key, key) == 0) {
      old = g_kept[i].toast;
      g_kept[i].toast = toast;
      free(key);
      key = NULL;
      break;
    }
  }
  if (key != NULL) {
    if (g_count == g_capacity) {
      size_t capacity = g_capacity ? g_capacity * 2 : 16;
      Kept* grown = (Kept*)realloc(g_kept, capacity * sizeof(Kept));
      if (grown == NULL) {
        ReleaseSRWLockExclusive(&g_kept_lock);
        free(key);
        return toast; /* not kept: the caller releases it */
      }
      g_kept = grown;
      g_capacity = capacity;
    }
    g_kept[g_count].key = key;
    g_kept[g_count].toast = toast;
    g_count++;
  }
  ReleaseSRWLockExclusive(&g_kept_lock);
  return old;
}

/* Takes out the kept toasts whose key starts with `prefix` (all of them for an
 * app: "<app id>\n"; one: its whole key) into `out` (at most `max`). */
static size_t TakeKept(const wchar_t* prefix, int exact, IToast** out, size_t max) {
  size_t n = 0;
  size_t len = wcslen(prefix);
  AcquireSRWLockExclusive(&g_kept_lock);
  for (size_t i = 0; i < g_count && n < max;) {
    int match = exact ? wcscmp(g_kept[i].key, prefix) == 0
                      : wcsncmp(g_kept[i].key, prefix, len) == 0;
    if (match) {
      out[n++] = g_kept[i].toast;
      free(g_kept[i].key);
      g_kept[i] = g_kept[--g_count];
    } else {
      i++;
    }
  }
  ReleaseSRWLockExclusive(&g_kept_lock);
  return n;
}

/* --- the manager ------------------------------------------------------------------- */

static HRESULT Statics(IToastStatics** out) {
  HSTRING name = Hs(L"Windows.UI.Notifications.ToastNotificationManager");
  HRESULT hr = g_get_factory(
      name, &IID___x_ABI_CWindows_CUI_CNotifications_CIToastNotificationManagerStatics,
      (void**)out);
  HsFree(name);
  return hr;
}

static HRESULT Notifier(const wchar_t* app_id, IToastNotifier** out) {
  IToastStatics* statics = NULL;
  HRESULT hr = Statics(&statics);
  if (FAILED(hr)) return hr;
  HSTRING id = Hs(app_id);
  hr = statics->lpVtbl->CreateToastNotifierWithId(statics, id, out);
  HsFree(id);
  RELEASE(statics);
  return hr;
}

static HRESULT History(IToastHistory** out) {
  IToastStatics* statics = NULL;
  IToastStatics2* statics2 = NULL;
  HRESULT hr = Statics(&statics);
  if (FAILED(hr)) return hr;
  hr = QUERY(statics, IID___x_ABI_CWindows_CUI_CNotifications_CIToastNotificationManagerStatics2,
             &statics2);
  if (SUCCEEDED(hr)) hr = statics2->lpVtbl->get_History(statics2, out);
  RELEASE(statics2);
  RELEASE(statics);
  return hr;
}

static void Hide(const wchar_t* app_id, IToast* toast) {
  IToastNotifier* notifier = NULL;
  if (SUCCEEDED(Notifier(app_id, &notifier))) {
    notifier->lpVtbl->Hide(notifier, toast);
    RELEASE(notifier);
  }
}

long gesher_toast_show(const wchar_t* app_id, const wchar_t* tag, const wchar_t* group,
                       const wchar_t* xml, int suppress_popup, int high) {
  if (!RtOk()) return E_NOTIMPL;
  IInspectable* doc_object = NULL;
  IXmlDocIO* io = NULL;
  IXmlDoc* doc = NULL;
  IToastFactory* factory = NULL;
  IToast* toast = NULL;
  IToast2* toast2 = NULL;
  IToast4* toast4 = NULL;
  IToastNotifier* notifier = NULL;
  HSTRING h = NULL;

  h = Hs(L"Windows.Data.Xml.Dom.XmlDocument");
  HRESULT hr = g_activate(h, &doc_object);
  HsFree(h);
  if (SUCCEEDED(hr)) {
    hr = QUERY(doc_object, IID___x_ABI_CWindows_CData_CXml_CDom_CIXmlDocumentIO, &io);
  }
  if (SUCCEEDED(hr)) {
    h = Hs(xml);
    hr = io->lpVtbl->LoadXml(io, h);
    HsFree(h);
  }
  if (SUCCEEDED(hr)) {
    hr = QUERY(doc_object, IID___x_ABI_CWindows_CData_CXml_CDom_CIXmlDocument, &doc);
  }
  if (SUCCEEDED(hr)) {
    h = Hs(L"Windows.UI.Notifications.ToastNotification");
    hr = g_get_factory(h, &IID___x_ABI_CWindows_CUI_CNotifications_CIToastNotificationFactory,
                       (void**)&factory);
    HsFree(h);
  }
  if (SUCCEEDED(hr)) hr = factory->lpVtbl->CreateToastNotification(factory, doc, &toast);
  if (SUCCEEDED(hr)) {
    hr = QUERY(toast, IID___x_ABI_CWindows_CUI_CNotifications_CIToastNotification2, &toast2);
  }
  if (SUCCEEDED(hr)) {
    h = Hs(tag);
    hr = toast2->lpVtbl->put_Tag(toast2, h);
    HsFree(h);
  }
  if (SUCCEEDED(hr)) {
    h = Hs(group);
    hr = toast2->lpVtbl->put_Group(toast2, h);
    HsFree(h);
  }
  if (SUCCEEDED(hr) && suppress_popup) toast2->lpVtbl->put_SuppressPopup(toast2, TRUE);
  if (SUCCEEDED(hr) && high &&
      SUCCEEDED(QUERY(toast, IID___x_ABI_CWindows_CUI_CNotifications_CIToastNotification4,
                      &toast4))) {
    toast4->lpVtbl->put_Priority(toast4,
                                 ToastNotificationPriority_High);
  }
  if (SUCCEEDED(hr)) {
    ActivatedHandler* handler = NewActivatedHandler(app_id);
    if (handler != NULL) {
      EventRegistrationToken token;
      toast->lpVtbl->add_Activated(toast, handler, &token);
      handler->lpVtbl->Release(handler);
    }
    hr = Notifier(app_id, &notifier);
  }
  if (SUCCEEDED(hr)) hr = notifier->lpVtbl->Show(notifier, toast);
  if (SUCCEEDED(hr)) {
    IToast* old = Keep(Key(app_id, tag, group), toast);
    toast = NULL; /* kept */
    RELEASE(old);
  }
  RELEASE(notifier);
  RELEASE(toast4);
  RELEASE(toast2);
  RELEASE(toast);
  RELEASE(factory);
  RELEASE(doc);
  RELEASE(io);
  RELEASE(doc_object);
  return hr;
}

long gesher_toast_remove(const wchar_t* app_id, const wchar_t* tag, const wchar_t* group) {
  if (!RtOk()) return E_NOTIMPL;
  wchar_t* key = Key(app_id, tag, group);
  IToast* kept[1];
  size_t n = key != NULL ? TakeKept(key, 1, kept, 1) : 0;
  free(key);
  for (size_t i = 0; i < n; i++) {
    Hide(app_id, kept[i]); /* the popup, if it is still up */
    RELEASE(kept[i]);
  }
  IToastHistory* history = NULL;
  HRESULT hr = History(&history);
  if (SUCCEEDED(hr)) {
    HSTRING t = Hs(tag), g = Hs(group), id = Hs(app_id);
    hr = history->lpVtbl->RemoveGroupedTagWithId(history, t, g, id);
    HsFree(t);
    HsFree(g);
    HsFree(id);
  }
  RELEASE(history);
  return hr;
}

long gesher_toast_clear(const wchar_t* app_id) {
  if (!RtOk()) return E_NOTIMPL;
  size_t len = wcslen(app_id);
  wchar_t* prefix = (wchar_t*)malloc((len + 2) * sizeof(wchar_t));
  if (prefix == NULL) return E_OUTOFMEMORY;
  wcscpy(prefix, app_id);
  wcscat(prefix, L"\n");
  IToast* kept[64];
  size_t n;
  while ((n = TakeKept(prefix, 0, kept, 64)) > 0) {
    for (size_t i = 0; i < n; i++) {
      Hide(app_id, kept[i]);
      RELEASE(kept[i]);
    }
  }
  free(prefix);
  IToastHistory* history = NULL;
  HRESULT hr = History(&history);
  if (SUCCEEDED(hr)) {
    HSTRING id = Hs(app_id);
    hr = history->lpVtbl->ClearWithId(history, id);
    HsFree(id);
  }
  RELEASE(history);
  return hr;
}
