/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
#include "store_entitlement.h"

#include <windows.h>
#include <appmodel.h>
#include <asyncinfo.h>
#include <roapi.h>
#include <wrl/client.h>
#include <winstring.h>

#include <array>
#include <string_view>

namespace gesher::store {
namespace {

using Microsoft::WRL::ComPtr;

// Store-specific interfaces are absent from the cross-compiler's headers.
// These prefixes follow the Windows SDK 10.0.26100.0 windows.services.store
// ABI, including the specialized Windows.Foundation async and map interfaces.
struct License : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_SkuStoreId(HSTRING*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsActive(boolean*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ExpirationDate(INT64*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ExtendedJsonData(HSTRING*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_InAppOfferToken(HSTRING*) = 0;
};
struct LicenseMap : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE Lookup(HSTRING, License**) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32*) = 0;
  virtual HRESULT STDMETHODCALLTYPE HasKey(HSTRING, boolean*) = 0;
};
struct AppLicense : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_SkuStoreId(HSTRING*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsActive(boolean*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_IsTrial(boolean*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ExpirationDate(INT64*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_ExtendedJsonData(HSTRING*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_AddOnLicenses(LicenseMap**) = 0;
};
struct LicenseOperation : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown*) = 0;
  virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetResults(AppLicense**) = 0;
};
struct Context : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_User(IInspectable**) = 0;
  virtual HRESULT STDMETHODCALLTYPE add_OfflineLicensesChanged(IUnknown*, INT64*) = 0;
  virtual HRESULT STDMETHODCALLTYPE remove_OfflineLicensesChanged(INT64) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCustomerPurchaseIdAsync(HSTRING, HSTRING, IInspectable**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCustomerCollectionsIdAsync(HSTRING, HSTRING, IInspectable**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetAppLicenseAsync(LicenseOperation**) = 0;
};
struct ContextStatics : IInspectable {
  virtual HRESULT STDMETHODCALLTYPE GetDefault(Context**) = 0;
};
constexpr GUID kContextStaticsId{0x9c06ee5f, 0x15c0, 0x4e72, {0x93, 0x30, 0xd6, 0x19, 0x1c, 0xeb, 0xd1, 0x9c}};

// Fill from the same reviewed release catalog as the checkout provider.
// Leave activation disabled until the release launcher proves containment.
constexpr std::wstring_view kPackageFamily = L"";
constexpr std::wstring_view kAddOnSku = L"";
constexpr std::wstring_view kOfferToken = L"";
constexpr bool kEditionReleaseEnabled = false;

struct Apartment {
  HRESULT status = RoInitialize(RO_INIT_MULTITHREADED);
  ~Apartment() { if (SUCCEEDED(status)) RoUninitialize(); }
};

class String {
 public:
  String() = default;
  String(const String&) = delete;
  String& operator=(const String&) = delete;
  ~String() { WindowsDeleteString(value); }

  bool Create(std::wstring_view text) {
    return SUCCEEDED(WindowsCreateString(text.data(), static_cast<UINT32>(text.size()), &value));
  }

  bool Equals(std::wstring_view expected) const {
    UINT32 length = 0;
    const wchar_t* raw = WindowsGetStringRawBuffer(value, &length);
    return raw && std::wstring_view(raw, length) == expected;
  }

  HSTRING value = nullptr;
};

bool MatchesCurrentPackage() {
  // PACKAGE_FAMILY_NAME_MAX_LENGTH in the Windows SDK is 64. MinGW's
  // appmodel.h declares the API without defining that SDK constant.
  std::array<wchar_t, 65> family{};
  UINT32 size = static_cast<UINT32>(family.size());
  if (GetCurrentPackageFamilyName(&size, family.data()) != ERROR_SUCCESS) return false;
  return CompareStringOrdinal(family.data(), -1, kPackageFamily.data(),
                              static_cast<int>(kPackageFamily.size()), TRUE) == CSTR_EQUAL;
}

bool WaitForLicense(LicenseOperation& operation) {
  ComPtr<IAsyncInfo> info;
  if (FAILED(operation.QueryInterface(__uuidof(IAsyncInfo),
                                      reinterpret_cast<void**>(info.GetAddressOf()))) || !info) {
    return false;
  }
  constexpr ULONGLONG kTimeoutMs = 15000;
  const ULONGLONG started = GetTickCount64();
  for (;;) {
    AsyncStatus status = Started;
    if (FAILED(info->get_Status(&status))) return false;
    if (status != Started) return status == Completed;
    if (GetTickCount64() - started >= kTimeoutMs) {
      info->Cancel();
      return false;
    }
    Sleep(25);
  }
}

}  // namespace

bool EditionReleaseEnabled() { return kEditionReleaseEnabled; }

Entitlement ReadEntitlement() {
  if (kPackageFamily.empty() || kAddOnSku.empty() || kOfferToken.empty()) return Entitlement::kUnconfigured;
  if (!MatchesCurrentPackage()) return Entitlement::kUnavailable;

  Apartment apartment;
  if (FAILED(apartment.status)) return Entitlement::kUnavailable;
  String className;
  String key;
  if (!className.Create(L"Windows.Services.Store.StoreContext") || !key.Create(kAddOnSku)) {
    return Entitlement::kUnavailable;
  }

  ComPtr<ContextStatics> factory;
  ComPtr<Context> context;
  ComPtr<LicenseOperation> operation;
  ComPtr<AppLicense> app;
  ComPtr<LicenseMap> addOns;
  if (FAILED(RoGetActivationFactory(className.value, kContextStaticsId,
                                    reinterpret_cast<void**>(factory.GetAddressOf()))) || !factory ||
      FAILED(factory->GetDefault(context.GetAddressOf())) || !context ||
      FAILED(context->GetAppLicenseAsync(operation.GetAddressOf())) || !operation ||
      !WaitForLicense(*operation.Get()) ||
      FAILED(operation->GetResults(app.GetAddressOf())) || !app ||
      FAILED(app->get_AddOnLicenses(addOns.GetAddressOf())) || !addOns) {
    return Entitlement::kUnavailable;
  }

  boolean present = false;
  if (FAILED(addOns->HasKey(key.value, &present))) return Entitlement::kUnavailable;
  if (!present) return Entitlement::kInactive;
  ComPtr<License> entry;
  String sku;
  String offer;
  if (FAILED(addOns->Lookup(key.value, entry.GetAddressOf())) || !entry ||
      FAILED(entry->get_SkuStoreId(&sku.value)) || !sku.Equals(kAddOnSku) ||
      FAILED(entry->get_InAppOfferToken(&offer.value)) || !offer.Equals(kOfferToken)) {
    return Entitlement::kUnavailable;
  }
  // The map contains only currently valid durables. StoreLicense.IsActive
  // is reserved and the base app's license is insufficient.
  return Entitlement::kActive;
}

}  // namespace gesher::store
