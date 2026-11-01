/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "drop_target.h"

#include <jni.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace android {
class AndroidRuntime {
 public:
  static JavaVM* getJavaVM();
};
}  // namespace android

namespace gesher {
namespace windows {
namespace {

constexpr int kActionDragStarted = 1;
constexpr int kActionDragLocation = 2;
constexpr int kActionDrop = 3;
constexpr int kActionDragEnded = 4;
constexpr int kActionDragEntered = 5;
constexpr int kActionDragExited = 6;

static JavaVM* g_vm = nullptr;

JavaVM* ResolveJavaVM() {
  if (g_vm != nullptr) return g_vm;
  return android::AndroidRuntime::getJavaVM();
}

}  // namespace

void SetDropTargetJavaVM(JavaVM* vm) {
  g_vm = vm;
}

void DispatchDragToSession(HWND hwnd, int action, float x, float y,
                           const std::vector<std::wstring>* files,
                           const std::wstring& text) {
  JavaVM* vm = ResolveJavaVM();
  if (vm == nullptr) return;

  JNIEnv* env = nullptr;
  bool attached = false;
  jint res = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (res != JNI_OK) {
    JavaVMAttachArgs args = {JNI_VERSION_1_6, const_cast<char*>("gesher-drop-target"), nullptr};
    if (vm->AttachCurrentThreadAsDaemon(&env, &args) != JNI_OK || env == nullptr) {
      return;
    }
    attached = true;
  }

  jclass sessionClass = env->FindClass("com/gesher/app/window/WindowSession");
  if (sessionClass != nullptr) {
    jmethodID method = env->GetStaticMethodID(
        sessionClass, "onNativeDragEvent",
        "(JIFF[Ljava/lang/String;Ljava/lang/String;)V");
    if (method != nullptr) {
      jobjectArray fileArray = nullptr;
      if (files != nullptr && !files->empty()) {
        jclass strClass = env->FindClass("java/lang/String");
        fileArray = env->NewObjectArray(static_cast<jsize>(files->size()), strClass, nullptr);
        for (size_t i = 0; i < files->size(); i++) {
          jstring s = env->NewString(reinterpret_cast<const jchar*>((*files)[i].c_str()),
                                     static_cast<jsize>((*files)[i].size()));
          env->SetObjectArrayElement(fileArray, static_cast<jsize>(i), s);
          env->DeleteLocalRef(s);
        }
        env->DeleteLocalRef(strClass);
      }

      jstring textStr = nullptr;
      if (!text.empty()) {
        textStr = env->NewString(reinterpret_cast<const jchar*>(text.c_str()),
                                 static_cast<jsize>(text.size()));
      }

      env->CallStaticVoidMethod(sessionClass, method,
                               reinterpret_cast<jlong>(hwnd),
                               static_cast<jint>(action),
                               static_cast<jfloat>(x),
                               static_cast<jfloat>(y),
                               fileArray, textStr);

      if (fileArray != nullptr) env->DeleteLocalRef(fileArray);
      if (textStr != nullptr) env->DeleteLocalRef(textStr);
    }
    env->DeleteLocalRef(sessionClass);
  }

  if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }

  if (attached) {
    vm->DetachCurrentThread();
  }
}

GesherDropTarget::GesherDropTarget(HWND hwnd) : hwnd_(hwnd) {}

GesherDropTarget::~GesherDropTarget() = default;

STDMETHODIMP GesherDropTarget::QueryInterface(REFIID riid, void** ppv) {
  if (ppv == nullptr) return E_POINTER;
  if (riid == IID_IUnknown || riid == IID_IDropTarget) {
    *ppv = static_cast<IDropTarget*>(this);
    AddRef();
    return S_OK;
  }
  *ppv = nullptr;
  return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) GesherDropTarget::AddRef() {
  return ++ref_count_;
}

STDMETHODIMP_(ULONG) GesherDropTarget::Release() {
  ULONG count = --ref_count_;
  if (count == 0) {
    delete this;
  }
  return count;
}

bool GesherDropTarget::QuerySupported(IDataObject* pDataObj) {
  if (pDataObj == nullptr) return false;

  FORMATETC fmt_hdrop = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  if (pDataObj->QueryGetData(&fmt_hdrop) == S_OK) return true;

  FORMATETC fmt_unicode = {CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  if (pDataObj->QueryGetData(&fmt_unicode) == S_OK) return true;

  FORMATETC fmt_text = {CF_TEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  if (pDataObj->QueryGetData(&fmt_text) == S_OK) return true;

  return false;
}

STDMETHODIMP GesherDropTarget::DragEnter(IDataObject* pDataObj, DWORD, POINTL pt,
                                        DWORD* pdwEffect) {
  if (pdwEffect == nullptr) return E_POINTER;

  format_supported_ = QuerySupported(pDataObj);
  if (!format_supported_) {
    *pdwEffect = DROPEFFECT_NONE;
    return S_OK;
  }

  *pdwEffect = DROPEFFECT_COPY;

  POINT client_pt = {pt.x, pt.y};
  ScreenToClient(hwnd_, &client_pt);

  DispatchDragToSession(hwnd_, kActionDragStarted,
                        static_cast<float>(client_pt.x),
                        static_cast<float>(client_pt.y),
                        nullptr, L"");
  DispatchDragToSession(hwnd_, kActionDragEntered,
                        static_cast<float>(client_pt.x),
                        static_cast<float>(client_pt.y),
                        nullptr, L"");
  DispatchDragToSession(hwnd_, kActionDragLocation,
                        static_cast<float>(client_pt.x),
                        static_cast<float>(client_pt.y),
                        nullptr, L"");
  return S_OK;
}

STDMETHODIMP GesherDropTarget::DragOver(DWORD, POINTL pt, DWORD* pdwEffect) {
  if (pdwEffect == nullptr) return E_POINTER;

  if (!format_supported_) {
    *pdwEffect = DROPEFFECT_NONE;
    return S_OK;
  }

  *pdwEffect = DROPEFFECT_COPY;

  POINT client_pt = {pt.x, pt.y};
  ScreenToClient(hwnd_, &client_pt);

  DispatchDragToSession(hwnd_, kActionDragLocation,
                        static_cast<float>(client_pt.x),
                        static_cast<float>(client_pt.y),
                        nullptr, L"");
  return S_OK;
}

STDMETHODIMP GesherDropTarget::DragLeave() {
  if (format_supported_) {
    DispatchDragToSession(hwnd_, kActionDragExited, 0, 0, nullptr, L"");
    DispatchDragToSession(hwnd_, kActionDragEnded, 0, 0, nullptr, L"");
    format_supported_ = false;
  }
  return S_OK;
}

void GesherDropTarget::ExtractAndDispatchDrop(IDataObject* pDataObj, POINT client_pt) {
  if (pDataObj == nullptr) return;

  FORMATETC fmt_hdrop = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  if (pDataObj->QueryGetData(&fmt_hdrop) == S_OK) {
    STGMEDIUM medium = {};
    if (SUCCEEDED(pDataObj->GetData(&fmt_hdrop, &medium))) {
      HDROP hdrop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
      if (hdrop != nullptr) {
        UINT count = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> files;
        files.reserve(count);
        for (UINT i = 0; i < count; i++) {
          UINT len = DragQueryFileW(hdrop, i, nullptr, 0);
          if (len > 0) {
            std::wstring path(len, L'\0');
            DragQueryFileW(hdrop, i, path.data(), len + 1);
            files.push_back(std::move(path));
          }
        }
        GlobalUnlock(medium.hGlobal);
        DispatchDragToSession(hwnd_, kActionDrop,
                              static_cast<float>(client_pt.x),
                              static_cast<float>(client_pt.y),
                              &files, L"");
      }
      ReleaseStgMedium(&medium);
      return;
    }
  }

  FORMATETC fmt_unicode = {CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  if (pDataObj->QueryGetData(&fmt_unicode) == S_OK) {
    STGMEDIUM medium = {};
    if (SUCCEEDED(pDataObj->GetData(&fmt_unicode, &medium))) {
      const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(medium.hGlobal));
      if (p != nullptr) {
        size_t max_len = GlobalSize(medium.hGlobal) / sizeof(wchar_t);
        size_t len = 0;
        while (len < max_len && p[len] != L'\0') len++;
        std::wstring text(p, len);
        GlobalUnlock(medium.hGlobal);
        DispatchDragToSession(hwnd_, kActionDrop,
                              static_cast<float>(client_pt.x),
                              static_cast<float>(client_pt.y),
                              nullptr, text);
      }
      ReleaseStgMedium(&medium);
      return;
    }
  }

  FORMATETC fmt_text = {CF_TEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  if (pDataObj->QueryGetData(&fmt_text) == S_OK) {
    STGMEDIUM medium = {};
    if (SUCCEEDED(pDataObj->GetData(&fmt_text, &medium))) {
      const char* p = static_cast<const char*>(GlobalLock(medium.hGlobal));
      if (p != nullptr) {
        size_t size = GlobalSize(medium.hGlobal);
        size_t len = 0;
        while (len < size && p[len] != '\0') len++;
        int wlen = MultiByteToWideChar(CP_ACP, 0, p, static_cast<int>(len), nullptr, 0);
        std::wstring text(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_ACP, 0, p, static_cast<int>(len), text.data(), wlen);
        GlobalUnlock(medium.hGlobal);
        DispatchDragToSession(hwnd_, kActionDrop,
                              static_cast<float>(client_pt.x),
                              static_cast<float>(client_pt.y),
                              nullptr, text);
      }
      ReleaseStgMedium(&medium);
      return;
    }
  }
}

STDMETHODIMP GesherDropTarget::Drop(IDataObject* pDataObj, DWORD, POINTL pt,
                                   DWORD* pdwEffect) {
  if (pdwEffect == nullptr) return E_POINTER;

  if (!format_supported_) {
    *pdwEffect = DROPEFFECT_NONE;
    return S_OK;
  }

  POINT client_pt = {pt.x, pt.y};
  ScreenToClient(hwnd_, &client_pt);

  ExtractAndDispatchDrop(pDataObj, client_pt);
  DispatchDragToSession(hwnd_, kActionDragEnded,
                        static_cast<float>(client_pt.x),
                        static_cast<float>(client_pt.y),
                        nullptr, L"");

  format_supported_ = false;
  *pdwEffect = DROPEFFECT_COPY;
  return S_OK;
}

HRESULT RegisterWindowDropTarget(HWND hwnd) {
  if (hwnd == nullptr) return E_INVALIDARG;
  GesherDropTarget* target = new GesherDropTarget(hwnd);
  HRESULT hr = RegisterDragDrop(hwnd, target);
  target->Release();  // RegisterDragDrop increments the reference count
  return hr;
}

HRESULT RevokeWindowDropTarget(HWND hwnd) {
  if (hwnd == nullptr) return E_INVALIDARG;
  return RevokeDragDrop(hwnd);
}

}  // namespace windows
}  // namespace gesher
