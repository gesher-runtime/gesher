// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// A read-only, agile COM stream over bytes already owned by the WebView bridge.
#pragma once

#include <objbase.h>
#include <objidl.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace gesher::webview {

using SharedBody = std::shared_ptr<const std::vector<uint8_t>>;

// WebView2 can retain and read request/response content on a background thread.
// Only the cursor is mutable; clones share the bytes and have their own cursor.
// No apartment-bound interface or application object is retained by this stream.
class OwnedStream final : public IStream, public IAgileObject {
 public:
  static HRESULT Create(SharedBody body, IStream** out) {
    return CreateAt(std::move(body), 0, out);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (out == nullptr) return E_POINTER;
    *out = nullptr;
    if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_ISequentialStream) ||
        IsEqualIID(iid, IID_IStream)) {
      *out = static_cast<IStream*>(this);
    } else if (IsEqualIID(iid, IID_IAgileObject)) {
      *out = static_cast<IAgileObject*>(this);
    } else if (IsEqualIID(iid, IID_IMarshal) && marshaler_ != nullptr) {
      return marshaler_->QueryInterface(iid, out);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG left = --refs_;
    if (left == 0) delete this;
    return left;
  }

  HRESULT STDMETHODCALLTYPE Read(void* dst, ULONG count, ULONG* read) override {
    if (read != nullptr) *read = 0;
    if (dst == nullptr && count != 0) return STG_E_INVALIDPOINTER;
    std::lock_guard<std::mutex> lock(mutex_);
    const ULONG take = Available(count);
    if (take != 0) std::memcpy(dst, body_->data() + position_, take);
    position_ += take;
    if (read != nullptr) *read = take;
    return take == count ? S_OK : S_FALSE;
  }

  HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG* written) override {
    if (written != nullptr) *written = 0;
    return STG_E_ACCESSDENIED;
  }

  HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin,
                                  ULARGE_INTEGER* position) override {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t next;
    if (origin == STREAM_SEEK_SET) {
      // COM defines an absolute offset as unsigned, including the high bit.
      next = static_cast<uint64_t>(move.QuadPart);
    } else if (origin == STREAM_SEEK_CUR || origin == STREAM_SEEK_END) {
      const uint64_t base = origin == STREAM_SEEK_CUR ? position_ : Size();
      if (move.QuadPart < 0) {
        const uint64_t distance = static_cast<uint64_t>(-(move.QuadPart + 1)) + 1;
        if (distance > base) return STG_E_INVALIDFUNCTION;
        next = base - distance;
      } else {
        const uint64_t distance = static_cast<uint64_t>(move.QuadPart);
        if (distance > std::numeric_limits<uint64_t>::max() - base) return STG_E_INVALIDFUNCTION;
        next = base + distance;
      }
    } else {
      return STG_E_INVALIDFUNCTION;
    }
    position_ = next;
    if (position != nullptr) position->QuadPart = next;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }

  HRESULT STDMETHODCALLTYPE CopyTo(IStream* target, ULARGE_INTEGER count,
                                    ULARGE_INTEGER* read, ULARGE_INTEGER* written) override {
    if (read != nullptr) read->QuadPart = 0;
    if (written != nullptr) written->QuadPart = 0;
    if (target == nullptr) return STG_E_INVALIDPOINTER;
    uint64_t total = 0, total_written = 0;
    while (total < count.QuadPart) {
      const uint8_t* source;
      ULONG take;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        take = Available(static_cast<ULONG>(std::min<uint64_t>(count.QuadPart - total, 65536)));
        if (take == 0) break;
        source = body_->data() + position_;
        position_ += take;
      }
      // The immutable backing stays alive. Never call external COM while holding
      // the cursor lock: the target may reenter this stream or be this stream.
      total += take;
      if (read != nullptr) read->QuadPart = total;
      ULONG put = 0;
      HRESULT hr = target->Write(source, take, &put);
      if (put > take) return STG_E_WRITEFAULT;
      total_written += put;
      if (written != nullptr) written->QuadPart = total_written;
      if (FAILED(hr)) return hr;
      if (put != take) return STG_E_MEDIUMFULL;
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE Revert() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
    return STG_E_INVALIDFUNCTION;
  }
  HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
    return STG_E_INVALIDFUNCTION;
  }

  HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD flags) override {
    if (stat == nullptr) return STG_E_INVALIDPOINTER;
    *stat = {};
    if (flags != STATFLAG_DEFAULT && flags != STATFLAG_NONAME && flags != STATFLAG_NOOPEN) {
      return STG_E_INVALIDFLAG;
    }
    stat->type = STGTY_STREAM;
    stat->cbSize.QuadPart = Size();
    stat->grfMode = STGM_READ;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Clone(IStream** out) override {
    if (out == nullptr) return STG_E_INVALIDPOINTER;
    uint64_t position;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      position = position_;
    }
    HRESULT hr = CreateAt(body_, position, out);
    return hr == E_OUTOFMEMORY ? STG_E_INSUFFICIENTMEMORY : hr;
  }

 private:
  OwnedStream(SharedBody body, uint64_t position) : body_(std::move(body)), position_(position) {}
  ~OwnedStream() {
    if (marshaler_ != nullptr) marshaler_->Release();
  }

  static HRESULT CreateAt(SharedBody body, uint64_t position, IStream** out) {
    if (out == nullptr) return E_POINTER;
    *out = nullptr;
    auto* stream = new (std::nothrow) OwnedStream(std::move(body), position);
    if (stream == nullptr) return E_OUTOFMEMORY;
    HRESULT hr = CoCreateFreeThreadedMarshaler(static_cast<IStream*>(stream), &stream->marshaler_);
    if (FAILED(hr)) {
      stream->Release();
      return hr;
    }
    *out = stream;
    return S_OK;
  }

  // Called only with mutex_ held. Seeking beyond EOF never forms an invalid pointer.
  ULONG Available(ULONG count) const {
    if (position_ >= Size()) return 0;
    return static_cast<ULONG>(std::min<uint64_t>(count, Size() - position_));
  }

  // A null owner represents empty bytes without a separate backing allocation.
  uint64_t Size() const { return body_ ? body_->size() : 0; }

  std::atomic<ULONG> refs_{1};
  const SharedBody body_;
  std::mutex mutex_;
  uint64_t position_;
  IUnknown* marshaler_ = nullptr;
};

}  // namespace gesher::webview
