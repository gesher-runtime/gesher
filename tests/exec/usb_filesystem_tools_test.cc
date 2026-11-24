// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Runs the upstream Android PIE programs through the production ELF
// loader and POSIX block/sysfs paths. Only this dedicated test executable has a
// test bootstrap, backed by an isolated byte buffer; no physical device opens.
#define main GesherFilesystemGuestMain
#include "../../compat/elf/exec/gesher_exec.cc"
#undef main

#include "../../compat/usb_storage/block_broker.h"
#include "../../compat/usb_storage/block_channel.h"
#include <gesher/usb_block.h>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>

namespace fs_fixture {
using namespace gesher::usb_storage;

struct Close {
  void operator()(void* value) const { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
using Handle = std::unique_ptr<void, Close>;

constexpr size_t kMediaBytes = 128 * 1024 * 1024;
constexpr size_t kPartition = 1024 * 1024;
constexpr const char* kNode = "/dev/block/sdb1";

uint16_t U16(const uint8_t* at) { return at[0] | (uint16_t{at[1]} << 8); }
uint32_t U32(const uint8_t* at) { return U16(at) | (uint32_t{U16(at + 2)} << 16); }
void Put16(uint8_t* at, uint16_t value) { at[0] = value & 255; at[1] = value >> 8; }
void Put32(uint8_t* at, uint32_t value) { Put16(at, value & 65535); Put16(at + 2, value >> 16); }

struct Media {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(kMediaBytes);
  bool consent = true;
  bool readonly = false;
  DWORD flush_error = ERROR_SUCCESS;
  DWORD readonly_error = ERROR_SUCCESS;
  std::atomic<uint64_t> writes{0};
  std::atomic<uint64_t> flushes{0};
  explicit Media(uint8_t partition_type) {
    bytes[510] = 0x55; bytes[511] = 0xaa;
    bytes[446 + 4] = partition_type;
    Put32(bytes.data() + 446 + 8, kPartition / 512);
    Put32(bytes.data() + 446 + 12, (kMediaBytes - kPartition) / 512);
  }
};

bool Transfer(BlockChannel& channel, void* bytes, DWORD count, bool writing) {
  auto* cursor = static_cast<uint8_t*>(bytes);
  while (count) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return false;
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    DWORD done = 0;
    BOOL result = writing ? WriteFile(channel.server(), cursor, count, nullptr, &operation)
                          : ReadFile(channel.server(), cursor, count, nullptr, &operation);
    if (!result && GetLastError() == ERROR_IO_PENDING) {
      const HANDLE waits[] = {channel.revocation(), event.get()};
      if (WaitForMultipleObjects(2, waits, FALSE, 125000) != WAIT_OBJECT_0 + 1) {
        CancelIoEx(channel.server(), &operation);
        GetOverlappedResult(channel.server(), &operation, &done, TRUE);
        return false;
      }
      result = TRUE;
    }
    if (!result || !GetOverlappedResult(channel.server(), &operation, &done, FALSE) || !done || done > count) return false;
    cursor += done; count -= done;
  }
  return true;
}

bool Reply(BlockChannel& channel, DWORD status, void* data = nullptr, DWORD size = 0) {
  BlockReply reply{status, size};
  return Transfer(channel, &reply, sizeof(reply), true) && (!size || Transfer(channel, data, size, true));
}

bool Serve(BlockChannel& channel, Media& media) {
  std::vector<uint8_t> buffer(kBlockTransferLimit);
  for (;;) {
    BlockRequest request{};
    if (!Transfer(channel, &request, sizeof(request), false)) return true;  // exit or deliberate revocation
    if (request.version != kBlockProtocolVersion || request.reserved) return false;
    if (request.operation == BlockOperation::kDescribe) {
      if (request.length || request.offset) return false;
      BlockDescription info{kMediaBytes, 512, kBlockRemovable |
          (media.consent ? kBlockWriteConsent : 0) | (media.consent && !media.readonly ? kBlockWritable : 0), 0, 0};
      if (!Reply(channel, 0, &info, sizeof(info))) return true;
      continue;
    }
    if (request.operation == BlockOperation::kFlush) {
      if (request.length || request.offset) return false;
      ++media.flushes;
      if (!Reply(channel, media.flush_error)) return true;
      continue;
    }
    if (request.operation == BlockOperation::kSetReadOnly) {
      if (request.length || request.offset > 1) return false;
      const DWORD error = !media.consent ? ERROR_ACCESS_DENIED : media.readonly_error;
      if (!error) media.readonly = request.offset != 0;
      if (!Reply(channel, error)) return true;
      continue;
    }
    if (!request.length || request.length > kBlockTransferLimit || request.offset > media.bytes.size() ||
        request.length > media.bytes.size() - request.offset) return false;
    if (request.operation == BlockOperation::kRead) {
      if (!Reply(channel, 0, media.bytes.data() + request.offset, request.length)) return true;
    } else if (request.operation == BlockOperation::kWrite) {
      if (!Transfer(channel, buffer.data(), request.length, false)) return true;
      DWORD error = !media.consent ? ERROR_ACCESS_DENIED : media.readonly ? ERROR_WRITE_PROTECT : 0;
      if (!error) {
        std::memcpy(media.bytes.data() + request.offset, buffer.data(), request.length);
        ++media.writes;
      }
      if (!Reply(channel, error)) return true;
    } else return false;
  }
}

Handle TemporaryOutput() {
  wchar_t folder[MAX_PATH], path[MAX_PATH];
  if (!GetTempPathW(MAX_PATH, folder) || !GetTempFileNameW(folder, L"gfs", 0, path)) return {};
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
  HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
      &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (file == INVALID_HANDLE_VALUE) { DeleteFileW(path); return {}; }
  return Handle(file);
}

std::string Output(HANDLE file) {
  LARGE_INTEGER size{}, zero{};
  if (!GetFileSizeEx(file, &size) || !SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) return "<output read failed>";
  constexpr uint64_t maximum = 1024 * 1024;
  const auto count = static_cast<DWORD>(std::min<uint64_t>(size.QuadPart, maximum));
  std::string text(count, '\0');
  DWORD done = 0;
  if (count && !ReadFile(file, text.data(), count, &done, nullptr)) return "<output read failed>";
  text.resize(done);
  if (static_cast<uint64_t>(size.QuadPart) > maximum) text += "\n<output truncated>\n";
  return text;
}

struct Result { int code = -1; bool timeout = false; bool protocol = false; std::string output; };

Result Run(Media& media, const std::vector<std::string>& arguments, DWORD timeout = 120000) {
  std::atomic<bool> served{false};  // Outlives Child's thread, including failed resume.
  struct Child {
    PROCESS_INFORMATION process{};
    std::unique_ptr<BlockChannel> channel;
    std::thread server;
    ~Child() {
      if (channel) channel->Revoke();
      if (process.hProcess && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 137);
        WaitForSingleObject(process.hProcess, 5000);
      }
      if (server.joinable()) server.join();
      if (process.hThread) CloseHandle(process.hThread);
      if (process.hProcess) CloseHandle(process.hProcess);
    }
  } child;
  Result result;
  if (BlockChannel::Create(&child.channel)) return result;
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  HANDLE read = nullptr, write = nullptr;
  if (!CreatePipe(&read, &write, &inherit, 0)) return result;
  Handle input(read), bootstrap(write), output = TemporaryOutput();
  if (!output || !SetHandleInformation(bootstrap.get(), HANDLE_FLAG_INHERIT, 0)) return result;
  wchar_t executable[32768];
  const DWORD executable_size = GetModuleFileNameW(nullptr, executable, std::size(executable));
  if (!executable_size || executable_size >= std::size(executable)) return result;
  std::wstring command;
  ex::AppendQuoted(&command, executable);
  for (const auto& argument : arguments) { command.push_back(L' '); ex::AppendQuoted(&command, ex::Widen(argument)); }
  SIZE_T bytes = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
  std::vector<uint8_t> storage(bytes);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) return result;
  struct AttributeOwner {
    LPPROC_THREAD_ATTRIBUTE_LIST value;
    ~AttributeOwner() { DeleteProcThreadAttributeList(value); }
  } attribute_owner{attributes};
  HANDLE handles[] = {input.get(), output.get()};
  if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
      handles, sizeof(handles), nullptr, nullptr)) return result;
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = input.get();
  startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = output.get();
  startup.lpAttributeList = attributes;
  // The fixture identity exists only in this test-created ordinary process.
  // Production protected credentials are authority-authenticated independently.
  SetEnvironmentVariableW(L"GESHER_UID", L"0");
  const BOOL created = CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
      CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
      &startup.StartupInfo, &child.process);
  SetEnvironmentVariableW(L"GESHER_UID", nullptr);
  if (!created) return result;
  input.reset();
  BlockGuestHandles guest;
  if (child.channel->Handoff(child.process.hProcess, &guest)) return result;
  DWORD sent = 0;
  if (!WriteFile(bootstrap.get(), &guest, sizeof(guest), &sent, nullptr) || sent != sizeof(guest)) return result;
  bootstrap.reset();
  child.server = std::thread([&] { served = Serve(*child.channel, media); });
  if (ResumeThread(child.process.hThread) != 1) return result;
  const DWORD waited = WaitForSingleObject(child.process.hProcess, timeout);
  result.timeout = waited == WAIT_TIMEOUT;
  if (waited != WAIT_OBJECT_0) {
    TerminateProcess(child.process.hProcess, 137);
    WaitForSingleObject(child.process.hProcess, 5000);
  }
  DWORD status = 0;
  if (GetExitCodeProcess(child.process.hProcess, &status)) result.code = ex::ExitStatus(status);
  child.channel->Revoke();
  child.server.join();
  result.protocol = served.load();
  result.output = Output(output.get());
  std::printf("upstream tool %s: exit=%d timeout=%s\n%s\n", arguments.size() > 2 ? arguments[2].c_str() : "fixture",
      result.code, result.timeout ? "yes" : "no", result.output.c_str());
  return result;
}

std::vector<std::string> Command(const std::wstring& tools, const char* file,
    const char* argv0, std::initializer_list<const char*> options) {
  std::vector<std::string> command{"--elf", ex::Narrow(tools + L"\\" + ex::Widen(file)), argv0};
  for (const char* option : options) command.emplace_back(option);
  command.emplace_back(kNode);
  return command;
}

void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void Clean(const Result& result, const char* message) {
  Require(result.protocol && !result.timeout && result.code == 0, message);
}

void CheckMediaControls() {
  Media media(0x0c);
  auto result = Run(media, {"--tool", "sync"});
  Clean(result, "sync failed with an active writable fixture grant");
  Require(media.flushes > 0 && media.writes == 0, "sync did not reach the broker or changed media bytes");
  media.consent = false;
  uint64_t before = media.flushes.load();
  Clean(Run(media, {"--tool", "sync"}), "read-only session flush failed");
  Require(media.flushes > before && media.writes == 0, "read-only sync bypassed the broker");
  result = Run(media, {"--tool", "blockdev", "--setrw", kNode});
  Require(result.protocol && !result.timeout && result.code > 0,
      "blockdev cleared read-only state without write consent");
  media.consent = true;
  media.readonly = true;
  result = Run(media, {"--tool", "blockdev", "--getro", kNode});
  Clean(result, "blockdev did not query live media state");
  Require(result.output == "1\n", "blockdev misreported read-only media");
  media.readonly_error = ERROR_WRITE_PROTECT;
  result = Run(media, {"--tool", "blockdev", "--setrw", kNode});
  Require(result.protocol && !result.timeout && result.code > 0 && media.readonly,
      "blockdev claimed to remove hardware write protection");
  media.readonly_error = ERROR_SUCCESS;
  Clean(Run(media, {"--tool", "blockdev", "--setrw", kNode}), "software read-only clear failed");
  Require(!media.readonly && media.writes == 0, "blockdev did not apply the requested state faithfully");
  result = Run(media, {"--tool", "blockdev", "--getro", kNode});
  Clean(result, "blockdev did not re-query changed media state");
  Require(result.output == "0\n", "blockdev cached stale read-only state");
  for (DWORD error : {DWORD{ERROR_WRITE_FAULT}, DWORD{ERROR_DEVICE_NOT_CONNECTED}}) {
    before = media.flushes.load();
    media.flush_error = error;
    result = Run(media, {"--tool", "sync"});
    Require(result.protocol && !result.timeout && result.code > 0 && media.flushes > before,
        "sync hid an actual flush failure or removal");
  }
}

uint32_t SuperblockCrc(const uint8_t* bytes) {
  uint32_t crc = UINT32_MAX;
  for (size_t i = 0; i < 1020; ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0x82f63b78u : 0);
  }
  return crc;
}

void CheckFilesystems(const std::wstring& tools) {
  {
    Media denied(0x0c);
    denied.consent = false;
    const auto result = Run(denied, Command(tools, "mkfs.fat", "mkfs.vfat", {"-F", "32", "-n", "CW_TEST"}));
    Require(result.protocol && !result.timeout && result.code > 0 && denied.writes == 0,
        "formatter must fail without write consent and leave fixture bytes untouched");
  }
  unsigned failures = 0;
  for (const int filesystem : {0, 1, 2}) {
    try {
      Media media(filesystem == 0 ? 0x0c : filesystem == 1 ? 0x07 : 0x83);
      const auto format = filesystem == 0 ? Command(tools, "mkfs.fat", "mkfs.vfat", {"-F", "32", "-n", "CW_TEST"})
          : filesystem == 1 ? Command(tools, "mkfs.exfat", "mkfs.exfat", {"-n", "CW_TEST"})
          : Command(tools, "mke2fs", "mkfs.ext4", {"-F", "-L", "CW_TEST"});
      const auto repair = filesystem == 0 ? Command(tools, "fsck.fat", "fsck.vfat", {"-a", "-w"})
          : filesystem == 1 ? Command(tools, "fsck.exfat", "fsck.exfat", {"-y"})
          : Command(tools, "e2fsck", "e2fsck", {"-p", "-f"});
      Clean(Run(media, format), "upstream formatter failed");
      Require(media.writes > 0, "formatter reported success without writes");
      uint8_t* volume = media.bytes.data() + kPartition;
      Require(filesystem == 0 ? std::memcmp(volume + 82, "FAT32   ", 8) == 0
          : filesystem == 1 ? std::memcmp(volume + 3, "EXFAT   ", 8) == 0
          : U16(volume + 1024 + 56) == 0xef53, "formatter did not write the requested filesystem");
      Require(U32(media.bytes.data() + 446 + 8) == kPartition / 512 && media.bytes[510] == 0x55 && media.bytes[511] == 0xaa,
          "partition-relative format changed the original MBR");
      Clean(Run(media, repair), "upstream checker did not accept newly formatted media");

      uint32_t before = 0;
      size_t changed = 0;
      if (filesystem == 0) {
        changed = static_cast<size_t>(U16(volume + 48)) * 512 + 488;
        Require(changed + 4 < media.bytes.size() - kPartition, "invalid FAT32 FSInfo location");
        before = U32(volume + changed);
        Require(before != 0, "FAT32 free cluster count unexpectedly zero");
        Put32(volume + changed, 0);  // corrupt only the isolated free-cluster summary
      } else if (filesystem == 1) {
        Put16(volume + 106, U16(volume + 106) | 2);  // dirty flag is excluded from boot checksum
        Put16(volume + 12 * 512 + 106, U16(volume + 12 * 512 + 106) | 2);
      } else {
        uint8_t* super = volume + 1024;
        before = U32(super + 12);
        Require(before > 1, "ext4 free block count unexpectedly small");
        Put32(super + 12, before - 1);  // validly checksummed but wrong accounting
        if (U32(super + 100) & 0x400) Put32(super + 1020, SuperblockCrc(super));
      }
      const uint64_t writes_before = media.writes.load();
      const Result fixed = Run(media, repair);
      Require(fixed.protocol && !fixed.timeout && (fixed.code == 0 || fixed.code == 1), "upstream repair failed");
      Require(media.writes > writes_before, "repair reported success without correcting any bytes");
      Require(filesystem == 0 ? U32(volume + changed) == before
          : filesystem == 1 ? !(U16(volume + 106) & 2)
          : U32(volume + 1024 + 12) == before, "repair did not correct the introduced corruption");
      Clean(Run(media, repair), "repaired filesystem is not clean according to the upstream checker");
    } catch (const std::exception& failure) {
      static constexpr const char* names[] = {"FAT32", "exFAT", "ext4"};
      std::fprintf(stderr, "%s fixture failed: %s\n", names[filesystem], failure.what());
      ++failures;
    }
  }
  Media hanging(0x0c);
  const Result timeout = Run(hanging, {"--usb-fixture-hang"}, 100);
  Require(timeout.timeout && timeout.code == 137 && hanging.writes == 0,
      "timeout must terminate child and report failure without media writes");
  Require(failures == 0, "one or more upstream filesystem scenarios failed");
}

int Guest(bool hang) {
  BlockGuestHandles handles;
  DWORD received = 0;
  if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), &handles, sizeof(handles), &received, nullptr) ||
      received != sizeof(handles)) return 121;
  const int attached = gesher::usb_block::AttachBroker(handles.pipe, handles.gate, handles.revoked);
  for (HANDLE handle : {handles.pipe, handles.gate, handles.revoked}) CloseHandle(handle);
  if (attached) return 122;
  if (hang) { Sleep(INFINITE); return 123; }
  return GesherFilesystemGuestMain();
}
}  // namespace fs_fixture

int main() {
  int count = 0;
  wchar_t** raw = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!raw || count < 1) return 1;
  std::vector<std::wstring> args(raw, raw + count);
  LocalFree(raw);
  if (count >= 2 && (args[1] == L"--elf" || args[1] == L"--tool")) return fs_fixture::Guest(false);
  if (count == 2 && args[1] == L"--usb-fixture-hang") return fs_fixture::Guest(true);
  std::wstring tools;
  if (count == 3 && args[1] == L"--tools") tools = args[2];
  else if (count == 1) {
    tools = args[0].substr(0, args[0].find_last_of(L"\\/")) + L"\\filesystem-tools\\bin";
  } else return 2;
  try {
    fs_fixture::CheckMediaControls();
    fs_fixture::CheckFilesystems(tools);
    std::puts("Upstream FAT32/exFAT/ext4 format, check, repair and consent fixture passed");
    return 0;
  } catch (const std::exception& failure) {
    std::fprintf(stderr, "Upstream filesystem fixture failed: %s\n", failure.what());
    return 1;
  }
}
