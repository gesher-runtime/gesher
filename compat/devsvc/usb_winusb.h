// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>
#include <winusb.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace gesher::usb {

// The operating system owns driver binding. Only present interfaces whose
// devnodes already use WinUSB are candidates.
std::optional<std::vector<std::wstring>> InterfacePaths();
bool AttachmentMonitoringAvailable();
// Returns -1 if connection-change monitoring is unavailable.
int64_t AttachmentVersion(const wchar_t* path);

struct CloseFile {
  void operator()(void* handle) const { CloseHandle(handle); }
};
using File = std::unique_ptr<void, CloseFile>;

File OpenDevice(const wchar_t* path, bool exclusive = false);

// Borrows the file handle. Callers must keep it alive until the interface is
// destroyed, after waiting for any submitted operations.
class Connection {
 public:
  explicit Connection(HANDLE file);
  ~Connection();
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  explicit operator bool() const { return interface_ != nullptr; }
  std::vector<unsigned char> Descriptors() const;
  std::wstring StringDescriptor(unsigned char index) const;
  bool Claim(int number);
  bool Release(int number);
  bool SetAlternate(int number, int alternate);
  void Cancel();
  bool Submit(int endpoint, unsigned char* data, unsigned length, OVERLAPPED* operation);
  int Result(OVERLAPPED* operation, bool wait);
  bool CancelRequest(OVERLAPPED* operation);
  int Transfer(int endpoint, unsigned char* data, unsigned length, unsigned timeout);
  int Control(int type, int request, int value, int index, unsigned char* data,
              unsigned length, unsigned timeout);

 private:
  HANDLE file_;
  WINUSB_INTERFACE_HANDLE interface_ = nullptr;
  USB_INTERFACE_DESCRIPTOR descriptor_{};
  UCHAR alternate_index_ = 0;
  bool claimed_ = false;
  std::mutex submission_;
  bool closed_ = false;
  bool read_policies_set_ = false;
  bool ConfigureReadPipes();
  bool FindPipe(int endpoint) const;
};

}  // namespace gesher::usb
