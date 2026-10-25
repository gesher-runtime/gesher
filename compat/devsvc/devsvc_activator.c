/* Gesher: the toast activator (hwsvc2 track, ADR 0051): a COM class whose
 * INotificationActivationCallback Windows calls when a toast of a Gesher app
 * (or one of its buttons) is clicked, also after the Gesher device stopped.
 * Each app's AppUserModelID names it (CustomActivator, devsvc_win32.cc's
 * RegisterAppId); the installer registers it as a local server
 * (HKCU\Software\Classes\CLSID\{...}\LocalServer32: gesher-app toast-activated),
 * which COM starts when no process serves the class. The running system
 * server registers its class object (REGCLS_MULTIPLEUSE), the launcher COM
 * started registers one for a single activation; either way the click goes to
 * gesher_toast_activated, as the in-process Activated event does. In C, as
 * devsvc_toast.c. */

#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>

#include "devsvc_toast.h"

/* INotificationActivationCallback (notificationactivationcallback.h), declared here:
 * mingw-w64's header declares it only for NTDDI_VERSION >= NTDDI_WIN10. */
typedef struct {
  LPCWSTR Key;
  LPCWSTR Value;
} GesherUserInputData;

typedef struct GesherActivationCallback GesherActivationCallback;
typedef struct {
  HRESULT(STDMETHODCALLTYPE* QueryInterface)(GesherActivationCallback*, REFIID, void**);
  ULONG(STDMETHODCALLTYPE* AddRef)(GesherActivationCallback*);
  ULONG(STDMETHODCALLTYPE* Release)(GesherActivationCallback*);
  HRESULT(STDMETHODCALLTYPE* Activate)(GesherActivationCallback*, LPCWSTR app_user_model_id,
                                       LPCWSTR invoked_args, const GesherUserInputData* data,
                                       ULONG count);
} GesherActivationCallbackVtbl;
struct GesherActivationCallback {
  const GesherActivationCallbackVtbl* lpVtbl;
};
typedef GesherActivationCallback INotificationActivationCallback;
typedef GesherActivationCallbackVtbl INotificationActivationCallbackVtbl;
typedef GesherUserInputData NOTIFICATION_USER_INPUT_DATA;

/* {53E31837-6600-4A81-9395-75CFFE746F94} */
static const IID IID_INotificationActivationCallback = {
    0x53E31837, 0x6600, 0x4A81, {0x93, 0x95, 0x75, 0xCF, 0xFE, 0x74, 0x6F, 0x94}};

/* {C18D9445-A7F0-472E-985F-AC354EBEB660}: Gesher's toast activator (gesher.iss). */
static const CLSID kGesherToastActivator = {
    0xC18D9445, 0xA7F0, 0x472E, {0x98, 0x5F, 0xAC, 0x35, 0x4E, 0xBE, 0xB6, 0x60}};

/* --- the callback: one static object ------------------------------------------- */

static HRESULT STDMETHODCALLTYPE CallbackQuery(INotificationActivationCallback* self,
                                               REFIID iid, void** out) {
  if (out == NULL) return E_POINTER;
  if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_INotificationActivationCallback)) {
    *out = self;
    return S_OK;
  }
  *out = NULL;
  return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE CallbackAddRef(INotificationActivationCallback* self) {
  (void)self;
  return 2;
}

static ULONG STDMETHODCALLTYPE CallbackRelease(INotificationActivationCallback* self) {
  (void)self;
  return 1;
}

static HRESULT STDMETHODCALLTYPE CallbackActivate(INotificationActivationCallback* self,
                                                  LPCWSTR app_user_model_id,
                                                  LPCWSTR invoked_args,
                                                  const NOTIFICATION_USER_INPUT_DATA* data,
                                                  ULONG count) {
  (void)self;
  (void)data;
  (void)count;
  /* The tag and group are not given: the arguments name the notification. */
  gesher_toast_activated(app_user_model_id != NULL ? app_user_model_id : L"", L"", L"",
                         invoked_args != NULL ? invoked_args : L"");
  return S_OK;
}

static INotificationActivationCallbackVtbl g_callback_vtbl = {
    CallbackQuery, CallbackAddRef, CallbackRelease, CallbackActivate};
static INotificationActivationCallback g_callback = {&g_callback_vtbl};

/* --- its class factory ------------------------------------------------------------ */

static HRESULT STDMETHODCALLTYPE FactoryQuery(IClassFactory* self, REFIID iid, void** out) {
  if (out == NULL) return E_POINTER;
  if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IClassFactory)) {
    *out = self;
    return S_OK;
  }
  *out = NULL;
  return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE FactoryAddRef(IClassFactory* self) {
  (void)self;
  return 2;
}

static ULONG STDMETHODCALLTYPE FactoryRelease(IClassFactory* self) {
  (void)self;
  return 1;
}

static HRESULT STDMETHODCALLTYPE FactoryCreate(IClassFactory* self, IUnknown* outer,
                                               REFIID iid, void** out) {
  (void)self;
  if (out == NULL) return E_POINTER;
  *out = NULL;
  if (outer != NULL) return CLASS_E_NOAGGREGATION;
  return CallbackQuery(&g_callback, iid, out);
}

static HRESULT STDMETHODCALLTYPE FactoryLock(IClassFactory* self, BOOL lock) {
  (void)self;
  (void)lock;
  return S_OK;
}

static IClassFactoryVtbl g_factory_vtbl = {FactoryQuery, FactoryAddRef, FactoryRelease,
                                           FactoryCreate, FactoryLock};
static IClassFactory g_factory = {&g_factory_vtbl};

static SRWLOCK g_lock = SRWLOCK_INIT;
static DWORD g_cookie; /* guarded by g_lock; 0: not registered */

long gesher_activator_register(int single_use) {
  HRESULT hr = S_OK;
  AcquireSRWLockExclusive(&g_lock);
  if (g_cookie == 0) {
    DWORD cookie = 0;
    hr = CoRegisterClassObject(&kGesherToastActivator, (IUnknown*)&g_factory,
                               CLSCTX_LOCAL_SERVER,
                               single_use ? REGCLS_SINGLEUSE : REGCLS_MULTIPLEUSE, &cookie);
    if (SUCCEEDED(hr)) g_cookie = cookie;
  }
  ReleaseSRWLockExclusive(&g_lock);
  return hr;
}

void gesher_activator_revoke(void) {
  AcquireSRWLockExclusive(&g_lock);
  if (g_cookie != 0) {
    CoRevokeClassObject(g_cookie);
    g_cookie = 0;
  }
  ReleaseSRWLockExclusive(&g_lock);
}
