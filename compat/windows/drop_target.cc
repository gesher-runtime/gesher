/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "drop_target.h"

#include <jni.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <gesher/binder_endpoint.h>
#include <shlobj.h>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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
  HMODULE mod = GetModuleHandleW(nullptr);
  if (mod != nullptr) {
    auto fn = reinterpret_cast<JavaVM* (*)()>(
        GetProcAddress(mod, "_ZN7android14AndroidRuntime9getJavaVMEv"));
    if (fn != nullptr) {
      g_vm = fn();
      return g_vm;
    }
  }
  return nullptr;
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

// A packaged protected drop consumes the source's OLE content stream. The
// AppContainer never asks a medium-integrity process to open the CF_HDROP path.
// Keep the resulting seekable files for the receiving window's drag-access lifetime.
struct GesherDropTarget::DropFiles {
  std::wstring root;
  std::vector<std::wstring> directories;
  std::vector<std::wstring> paths;
  std::vector<HANDLE> retained;

  ~DropFiles() {
    for (HANDLE file : retained) CloseHandle(file);
    for (const auto& directory : directories) RemoveDirectoryW(directory.c_str());
    if (!root.empty()) RemoveDirectoryW(root.c_str());
  }

  bool Copy(IDataObject* source, const std::vector<std::wstring>& names) {
    std::vector<std::wstring> file_names;
    for (const auto& path : names) {
      const auto separator = path.find_last_of(L"/\\");
      file_names.push_back(separator == std::wstring::npos ? path : path.substr(separator + 1));
    }
    if (!MatchesDescriptors(source, file_names)) return false;
    wchar_t temp[MAX_PATH]{}, suffix[40]{};
    const DWORD length = GetTempPathW(ARRAYSIZE(temp), temp);
    GUID id{};
    if (!length || length >= ARRAYSIZE(temp) || FAILED(CoCreateGuid(&id)) ||
        !StringFromGUID2(id, suffix, ARRAYSIZE(suffix))) return false;
    std::wstring root_path = std::wstring(temp, length) + L"GesherDrop" + suffix;
    if (!CreateDirectoryW(root_path.c_str(), nullptr)) return false;
    root = std::move(root_path);
    const auto format = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileContents"));
    if (!format) return false;
    for (size_t i = 0; i < names.size(); ++i) {
      const auto& name = file_names[i];
      if (name.empty() || name == L"." || name == L".." ||
          name.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos ||
          name.back() == L'.' || name.back() == L' ') return false;
      const std::wstring directory = root + L"\\" + std::to_wstring(i);
      if (!CreateDirectoryW(directory.c_str(), nullptr)) return false;
      directories.push_back(directory);
      FORMATETC request{format, nullptr, DVASPECT_CONTENT, static_cast<LONG>(i), TYMED_ISTREAM};
      STGMEDIUM content{};
      if (FAILED(source->GetData(&request, &content))) return false;
      const bool copied = content.tymed == TYMED_ISTREAM && content.pstm &&
                          CopyStream(content.pstm, directory + L"\\" + name);
      ReleaseStgMedium(&content);
      if (!copied) return false;
    }
    return !paths.empty();
  }

 private:
  static bool MatchesDescriptors(IDataObject* source, const std::vector<std::wstring>& names) {
    // FileContents indices refer to FileGroupDescriptorW, so check that they
    // correspond to the files Windows offered via CF_HDROP.
    const auto format = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW"));
    if (!format) return false;
    FORMATETC request{format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (FAILED(source->GetData(&request, &medium))) return false;
    bool matches = false;
    if (medium.tymed == TYMED_HGLOBAL) {
      constexpr size_t header = offsetof(FILEGROUPDESCRIPTORW, fgd);
      const size_t bytes = GlobalSize(medium.hGlobal);
      const auto* group = static_cast<const FILEGROUPDESCRIPTORW*>(GlobalLock(medium.hGlobal));
      if (group && bytes >= header && group->cItems == names.size() &&
          group->cItems <= (bytes - header) / sizeof(FILEDESCRIPTORW)) {
        matches = true;
        for (size_t i = 0; i < names.size(); ++i) {
          const FILEDESCRIPTORW& file = group->fgd[i];
          const auto* end = std::find(std::begin(file.cFileName), std::end(file.cFileName), L'\0');
          const auto name_length = end - file.cFileName;
          if (end == std::end(file.cFileName) ||
              ((file.dwFlags & FD_ATTRIBUTES) && (file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ||
              CompareStringOrdinal(file.cFileName, static_cast<int>(name_length), names[i].c_str(),
                                   -1, TRUE) != CSTR_EQUAL) {
            matches = false;
            break;
          }
        }
      }
      if (group) GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return matches;
  }

  bool CopyStream(IStream* source, const std::wstring& path) {
    STATSTG metadata{};
    if (FAILED(source->Stat(&metadata, STATFLAG_NONAME))) return false;
    ULARGE_INTEGER remaining{};
    if (!GetDiskFreeSpaceExW(root.c_str(), &remaining, nullptr, nullptr) ||
        metadata.cbSize.QuadPart > remaining.QuadPart) return false;
    // The OS closes these handles even when the guest is terminated. Android
    // can reopen the file while this window presents it, with delete sharing.
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE | DELETE,
                              FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    if (GetFileType(file) != FILE_TYPE_DISK) {
      CloseHandle(file);
      return false;
    }
    paths.push_back(path);
    bool copied = true;
    std::array<BYTE, 64 * 1024> buffer{};
    uint64_t bytes = metadata.cbSize.QuadPart;
    while (bytes > 0) {
      const ULONG size = static_cast<ULONG>(std::min<uint64_t>(bytes, buffer.size()));
      ULONG got = 0;
      DWORD written = 0;
      if (FAILED(source->Read(buffer.data(), size, &got)) || got == 0 || got > size ||
          !WriteFile(file, buffer.data(), got, &written, nullptr) || written != got) {
        copied = false;
        break;
      }
      bytes -= written;
    }
    if (copied && (metadata.mtime.dwLowDateTime || metadata.mtime.dwHighDateTime)) {
      SetFileTime(file, nullptr, nullptr, &metadata.mtime);
    }
    if (copied) retained.push_back(file);
    else CloseHandle(file);
    return copied;
  }
};

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

// A path already accessible to this AppContainer needs no additional
// authorization or copy, including a directory in the package's own data.
bool CanReadDropPaths(const std::vector<std::wstring>& paths) {
  for (const auto& path : paths) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const bool disk = GetFileType(handle) == FILE_TYPE_DISK;
    CloseHandle(handle);
    if (!disk) return false;
  }
  return true;
}

bool GesherDropTarget::ExtractAndDispatchDrop(IDataObject* pDataObj, POINT client_pt) {
  if (pDataObj == nullptr) return false;

  FORMATETC fmt_hdrop = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  if (pDataObj->QueryGetData(&fmt_hdrop) == S_OK) {
    STGMEDIUM medium = {};
    if (SUCCEEDED(pDataObj->GetData(&fmt_hdrop, &medium))) {
      std::vector<std::wstring> files;
      HDROP hdrop = medium.tymed == TYMED_HGLOBAL ? static_cast<HDROP>(GlobalLock(medium.hGlobal)) : nullptr;
      if (hdrop != nullptr) {
        const UINT count = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
        files.reserve(count);
        for (UINT i = 0; i < count; i++) {
          const UINT len = DragQueryFileW(hdrop, i, nullptr, 0);
          if (!len) break;
          std::wstring path(len + 1, L'\0');
          if (DragQueryFileW(hdrop, i, path.data(), len + 1) != len) break;
          path.resize(len);
          files.push_back(std::move(path));
        }
        GlobalUnlock(medium.hGlobal);
        if (files.size() != count) files.clear();
      }
      ReleaseStgMedium(&medium);
      if (files.empty()) return false;
      const auto* dropped = &files;
      if (binder_endpoint::Current().IsGuest() && !CanReadDropPaths(files)) {
        auto drop = std::make_unique<DropFiles>();
        if (!drop->Copy(pDataObj, files)) return false;
        dropped = &drop->paths;
        file_drops_.push_back(std::move(drop));
      }
      DispatchDragToSession(hwnd_, kActionDrop, static_cast<float>(client_pt.x),
                            static_cast<float>(client_pt.y), dropped, L"");
      return true;
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
      return p != nullptr;
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
      return p != nullptr;
    }
  }
  return false;
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

  const bool received = ExtractAndDispatchDrop(pDataObj, client_pt);
  DispatchDragToSession(hwnd_, kActionDragEnded,
                        static_cast<float>(client_pt.x),
                        static_cast<float>(client_pt.y),
                        nullptr, L"");

  format_supported_ = false;
  *pdwEffect = received ? DROPEFFECT_COPY : DROPEFFECT_NONE;
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
