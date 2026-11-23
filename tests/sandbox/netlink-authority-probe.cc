// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string>

#include "launch_channel.h"
#include "services_zygote.h"
#include <gesher/binder_endpoint.h>

namespace {
namespace fs = std::filesystem;
namespace channel = gesher::binder_launch;
namespace zygote = gesher::zygote;

bool Check(bool passed, const char* what) {
  std::printf("%s: %s (errno=%d)\n", what, passed ? "ok" : "FAILED", errno);
  std::fflush(stdout);
  return passed;
}

std::u16string Quote(const fs::path& path) { return u"\"" + path.u16string() + u"\""; }

bool WaitExit(int pid) {
  if (pid <= 0) return false;
  const auto deadline = GetTickCount64() + 15000;
  do {
    const int status = zygote::ExitCode(pid);
    if (status != STILL_ACTIVE) return status == 0;
    Sleep(20);
  } while (GetTickCount64() < deadline);
  return false;
}

struct LinkRequest {
  nlmsghdr header{NLMSG_LENGTH(sizeof(ifinfomsg)), RTM_GETLINK, NLM_F_REQUEST | NLM_F_DUMP, 37, 0};
  ifinfomsg link{};
};
static_assert(sizeof(LinkRequest) == NLMSG_LENGTH(sizeof(ifinfomsg)));

bool Denied(int fd) {
  sockaddr_nl local{};
  local.nl_family = AF_NETLINK;
  LinkRequest request;
  const bool bind_denied = bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) < 0 && errno == EACCES;
  const bool dump_denied = send(fd, &request, sizeof(request), 0) < 0 && errno == EACCES;
  return bind_denied && dump_denied;
}

bool ReadLinks(int fd, uint32_t port, int* first_index) {
  LinkRequest request;
  if (send(fd, &request, sizeof(request), 0) != static_cast<ssize_t>(sizeof(request))) return false;
  bool saw_interface = false;
  alignas(nlmsghdr) std::array<char, 4096> packet{};
  for (;;) {
    const auto received = recv(fd, packet.data(), packet.size(), MSG_DONTWAIT);
    if (received <= 0) return false;
    int remaining = static_cast<int>(received);
    for (auto* header = reinterpret_cast<nlmsghdr*>(packet.data()); NLMSG_OK(header, remaining);
         header = NLMSG_NEXT(header, remaining)) {
      if (header->nlmsg_pid != port || header->nlmsg_seq != request.header.nlmsg_seq) return false;
      if (header->nlmsg_type == NLMSG_DONE) return saw_interface;
      if (header->nlmsg_type != RTM_NEWLINK || header->nlmsg_len < NLMSG_LENGTH(sizeof(ifinfomsg))) return false;
      const auto* link = static_cast<const ifinfomsg*>(NLMSG_DATA(header));
      char name[IF_NAMESIZE];
      if (link->ifi_index <= 0 || !if_indextoname(static_cast<unsigned>(link->ifi_index), name)) return false;
      bool named = false;
      int attribute_bytes = header->nlmsg_len - NLMSG_LENGTH(sizeof(ifinfomsg));
      auto* attributes = reinterpret_cast<const rtattr*>(link + 1);
      for (; RTA_OK(attributes, attribute_bytes); attributes = RTA_NEXT(attributes, attribute_bytes)) {
        if (attributes->rta_type == IFLA_IFNAME &&
            static_cast<size_t>(RTA_PAYLOAD(attributes)) == std::strlen(name) + 1 &&
            std::memcmp(RTA_DATA(attributes), name, std::strlen(name) + 1) == 0) named = true;
      }
      if (!named || attribute_bytes != 0) return false;
      if (!saw_interface) *first_index = link->ifi_index;
      saw_interface = true;
    }
  }
}

bool ReadLink(int fd, uint32_t port, int index) {
  LinkRequest request;
  request.header.nlmsg_flags = NLM_F_REQUEST;
  request.link.ifi_index = index;
  if (send(fd, &request, sizeof(request), 0) != static_cast<ssize_t>(sizeof(request))) return false;
  alignas(nlmsghdr) std::array<char, 4096> packet{};
  const auto received = recv(fd, packet.data(), packet.size(), MSG_DONTWAIT);
  if (received < static_cast<ssize_t>(NLMSG_LENGTH(sizeof(ifinfomsg)))) return false;
  const auto* header = reinterpret_cast<const nlmsghdr*>(packet.data());
  const auto* link = static_cast<const ifinfomsg*>(NLMSG_DATA(header));
  return header->nlmsg_type == RTM_NEWLINK && header->nlmsg_len == static_cast<uint32_t>(received) &&
         header->nlmsg_pid == port && header->nlmsg_seq == request.header.nlmsg_seq &&
         (header->nlmsg_flags & NLM_F_MULTI) == 0 && link->ifi_index == index;
}

bool CheckInterfaceAddresses(bool expect_link_layer) {
  ifaddrs* addresses = nullptr;
  if (getifaddrs(&addresses) != 0) return false;
  bool saw_ip = false;
  bool saw_link_layer = false;
  for (const auto* address = addresses; address; address = address->ifa_next) {
    if (address->ifa_addr == nullptr) continue;
    const auto family = address->ifa_addr->sa_family;
    saw_ip |= (family == AF_INET || family == AF_INET6) && address->ifa_name != nullptr;
    saw_link_layer |= family == AF_PACKET;
  }
  freeifaddrs(addresses);
  return saw_ip && saw_link_layer == expect_link_layer;
}

bool ActivateInspection(HANDLE pipe) {
  gesher::process_identity::Process self;
  if (!gesher::process_identity::ReadLiveProcess(GetCurrentProcess(), &self)) return false;
  channel::Request request;
  request.action = channel::Action::kEnableNetlinkInspection;
  request.pid = self.pid;
  request.birth = self.birth;
  const auto deadline = GetTickCount64() + 5000;
  uint32_t status = ERROR_ACCESS_DENIED;
  return channel::Transfer(pipe, &request, sizeof(request), true, deadline) &&
         channel::Transfer(pipe, &status, sizeof(status), false, deadline) && status == ERROR_SUCCESS;
}

int NoAuthority(bool contained) {
  gesher::process_identity::Process self;
  if (!gesher::process_identity::ReadLiveProcess(GetCurrentProcess(), &self) ||
      (self.principal.container.size != 0) != contained) return 1;
  int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_ROUTE);
  const bool okay = fd >= 0 && !gesher::binder_endpoint::MayInspectNetlink() && Denied(fd) &&
      Check(CheckInterfaceAddresses(false), "ordinary getifaddrs retains IP rows only");
  if (fd >= 0) close(fd);
  return Check(okay, contained ? "contained app denied" : "unregistered host denied") ? 0 : 1;
}

int SystemServer(const fs::path& directory, const std::u16string& profile) {
  channel::Startup startup;
  if (!Check(channel::ReadStartup(channel::Endpoint::kSystemServer, &startup) &&
             zygote::HasProtectedLaunchAuthority(), "protected process admitted")) return 1;
  int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
  if (!Check(fd >= 0 && Denied(fd) && CheckInterfaceAddresses(false),
             "admitted system_server has no grant by default")) return 1;
  if (!Check(ActivateInspection(channel::DecodeHandle(startup.channel)), "explicit private grant") ||
      !Check(gesher::binder_endpoint::MayInspectNetlink(), "authenticated grant received")) return 1;

  sockaddr_nl local{};
  local.nl_family = AF_NETLINK;
  local.nl_pid = GetCurrentProcessId();
  local.nl_groups = RTMGRP_IPV4_IFADDR;
  const uint32_t port = local.nl_pid;
  if (!Check(bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == 0,
             "authorized explicit port and group binding")) return 1;
  socklen_t length = sizeof(local);
  int first_index = 0;
  if (!Check(getsockname(fd, reinterpret_cast<sockaddr*>(&local), &length) == 0 &&
             local.nl_pid == port && local.nl_groups == RTMGRP_IPV4_IFADDR && ReadLinks(fd, port, &first_index),
             "authentic Windows link dump")) return 1;
  if (!Check(ReadLink(fd, port, first_index), "single interface read matches the dumped generation")) return 1;
  if (!Check(CheckInterfaceAddresses(true), "authorized getifaddrs exposes IP and link-layer rows")) return 1;
  int other = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
  const bool duplicate_port = other >= 0 &&
      bind(other, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) < 0 && errno == EADDRINUSE;
  if (other >= 0) close(other);
  close(fd);
  if (!Check(duplicate_port, "exact port collision rejected")) return 1;

  const fs::path executable(zygote::ExePath());
  if (zygote::EnsureAppContainer(profile, profile) != ERROR_SUCCESS ||
      zygote::GrantToAppContainer(executable.u16string(), profile, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE, false) != 0) {
    return 1;
  }
  std::array<uint8_t, 16> installation{1};
  const int app = zygote::SpawnProtectedApp(Quote(executable) + u" --guest", {u"GESHER_UID=1000"}, {},
      (directory / L"app.log").u16string(), {profile, profile, {u"gesherBinderClient"}}, 11001, installation, 1);
  const int host = zygote::Spawn(Quote(executable) + u" --host", {u"GESHER_UID=1000"}, {},
      (directory / L"host.log").u16string(), false);
  return Check(WaitExit(app) && WaitExit(host), "other process roles stay denied after enable") ? 0 : 1;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 2 && std::wstring(argv[1]) == L"--guest") return NoAuthority(true);
  if (argc == 2 && std::wstring(argv[1]) == L"--host") return NoAuthority(false);
  if (argc == 4 && std::wstring(argv[1]) == L"--server") return SystemServer(argv[2], fs::path(argv[3]).u16string());
  if (argc != 3) return 2;

  const fs::path runtime = fs::absolute(argv[1]);
  const fs::path dir = fs::absolute(argv[2]) / (L"gesher-netlink-" + std::to_wstring(GetCurrentProcessId()) +
                                             L"-" + std::to_wstring(GetTickCount64()));
  if (!fs::create_directory(dir)) return 1;
  const std::u16string name = dir.filename().u16string();
  const std::u16string profile = name + u".guest";
  const auto children = zygote::StartProtectedDevice(name,
      {Quote(runtime / L"binderd.exe"), Quote(runtime / L"servicemanager.exe"),
       Quote(fs::path(zygote::ExePath())) + u" --server " + Quote(dir) + u" " + profile},
      {(dir / L"broker.log").u16string(), (dir / L"manager.log").u16string(), (dir / L"server.log").u16string()},
      {u"GESHER_BINDER=" + name}, true);
  const bool okay = children.size() == 3 && WaitExit(children[2]);
  zygote::DeleteAppContainer(profile);
  std::wprintf(L"Fixture logs: %ls\n", dir.c_str());
  return Check(okay, "actual protected NETLINK authority") ? 0 : 1;
}
