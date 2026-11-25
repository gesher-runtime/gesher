// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Shared public API corpus: native Linux, whole-production host and the actual
// Windows posix_probe. A watch belongs to the inode, not the spelling of a link.
struct RoutingEvent {
  int wd;
  uint32_t mask;
  std::string name;
};

static std::vector<RoutingEvent> RoutingRead(int fd, int timeout = 1000) {
  std::vector<RoutingEvent> events;
  pollfd item{fd, POLLIN, 0};
  if (poll(&item, 1, timeout) != 1) return events;
  alignas(inotify_event) char bytes[4096];
  ssize_t size = read(fd, bytes, sizeof(bytes));
  for (size_t offset = 0; size > 0 && offset + sizeof(inotify_event) <= static_cast<size_t>(size);) {
    inotify_event event;
    memcpy(&event, bytes + offset, sizeof(event));
    if (sizeof(event) + event.len > static_cast<size_t>(size) - offset) break;
    events.push_back({event.wd, event.mask, event.len ? bytes + offset + sizeof(event) : ""});
    offset += sizeof(event) + event.len;
  }
  return events;
}

static bool RoutingHas(const std::vector<RoutingEvent>& events, int wd, uint32_t mask,
                       const char* name = "") {
  for (const auto& e : events) {
    if (e.wd == wd && e.mask == mask && e.name == name) return true;
  }
  return false;
}

static void InotifyWriteRouting(const std::string& root) {
  const std::string a = root + "/route-a", b = root + "/route-b";
  const std::string original = a + "/watched", alias = b + "/alias", moved = a + "/moved";
  route_check("routing first directory", mkdir(a.c_str(), 0700) == 0);
  route_check("routing second directory", mkdir(b.c_str(), 0700) == 0);
  int original_fd = open(original.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  route_check("routing original real file", original_fd >= 0);
  route_check("routing cross-directory hard link", link(original.c_str(), alias.c_str()) == 0);
  int alias_fd = open(alias.c_str(), O_RDWR);
  route_check("routing alias real file", alias_fd >= 0);
  if (original_fd < 0 || alias_fd < 0) return;

  int in = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  route_check("routing real observer", in >= 0);
  int wd = inotify_add_watch(in, original.c_str(), IN_MODIFY);
  route_check("routing original watch", wd > 0);
  route_check("routing same inode keeps descriptor", inotify_add_watch(in, alias.c_str(), IN_MODIFY) == wd);
  errno = 0;
  route_check("routing mask-create rejects existing identity",
              inotify_add_watch(in, alias.c_str(), IN_MODIFY | IN_MASK_CREATE) == -1 && errno == EEXIST);
  route_check("routing actual alias write", write(alias_fd, "a", 1) == 1);
  route_check("routing cross-directory inode notification", RoutingHas(RoutingRead(in), wd, IN_MODIFY));

  route_check("routing mask-add keeps existing events",
              inotify_add_watch(in, alias.c_str(), IN_ATTRIB | IN_MASK_ADD) == wd);
  route_check("routing write after mask-add", write(alias_fd, "b", 1) == 1);
  route_check("routing mask-add retains modify", RoutingHas(RoutingRead(in), wd, IN_MODIFY));
  route_check("routing explicit watch removal", inotify_rm_watch(in, wd) == 0);
  route_check("routing removal reports ignored", RoutingHas(RoutingRead(in), wd, IN_IGNORED));
  int renewed = inotify_add_watch(in, original.c_str(), IN_MODIFY);
  route_check("routing retired identity receives a new descriptor", renewed > 0 && renewed != wd);
  route_check("routing write after retirement", write(alias_fd, "c", 1) == 1);
  route_check("routing replacement descriptor receives publication", RoutingHas(RoutingRead(in), renewed, IN_MODIFY));
  route_check("routing observer close", close(in) == 0);

  in = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  wd = inotify_add_watch(in, original.c_str(), IN_MODIFY | IN_ONESHOT);
  route_check("routing oneshot admission", wd > 0);
  route_check("routing oneshot write", write(alias_fd, "d", 1) == 1);
  auto once = RoutingRead(in);
  route_check("routing oneshot inode event", RoutingHas(once, wd, IN_MODIFY));
  route_check("routing oneshot retires after event", RoutingHas(once, wd, IN_IGNORED));
  errno = 0;
  route_check("routing oneshot no longer removable", inotify_rm_watch(in, wd) == -1 && errno == EINVAL);
  renewed = inotify_add_watch(in, original.c_str(), IN_MODIFY);
  route_check("routing oneshot removes identity index", renewed > 0 && renewed != wd);
  route_check("routing oneshot replacement write", write(alias_fd, "e", 1) == 1);
  route_check("routing oneshot replacement receives event", RoutingHas(RoutingRead(in), renewed, IN_MODIFY));
  route_check("routing oneshot observer close", close(in) == 0);

  in = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  wd = inotify_add_watch(in, original.c_str(), IN_MODIFY);
  route_check("routing rename original pathname", rename(original.c_str(), moved.c_str()) == 0);
  int replacement = open(original.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  route_check("routing replacement is distinct", replacement >= 0);
  route_check("routing write replacement", replacement >= 0 && write(replacement, "f", 1) == 1);
  route_check("routing replacement does not impersonate inode", RoutingRead(in, 0).empty());
  route_check("routing old open handle remains original", write(original_fd, "g", 1) == 1);
  route_check("routing renamed old handle still observed", RoutingHas(RoutingRead(in), wd, IN_MODIFY));
  route_check("routing cross-directory alias still observed after rename", write(alias_fd, "h", 1) == 1 &&
              RoutingHas(RoutingRead(in), wd, IN_MODIFY));
  route_check("routing replacement handle close", close(replacement) == 0);
  route_check("routing renamed observer close", close(in) == 0);

  // Directory publication follows the written directory entry. File publication
  // still follows identity; a parent of a different hard link is not modified.
  in = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  int dir_a = inotify_add_watch(in, a.c_str(), IN_MODIFY);
  wd = inotify_add_watch(in, moved.c_str(), IN_MODIFY);
  int dir_b = inotify_add_watch(in, b.c_str(), IN_MODIFY);
  route_check("routing directory and file watches admitted", dir_a > 0 && wd > 0 && dir_b > 0);
  route_check("routing mixed watch write", write(alias_fd, "i", 1) == 1);
  auto mixed = RoutingRead(in);
  route_check("routing written parent receives alias name", RoutingHas(mixed, dir_b, IN_MODIFY, "alias"));
  route_check("routing file receives nameless inode event", RoutingHas(mixed, wd, IN_MODIFY));
  route_check("routing other hard-link parent is not modified", !RoutingHas(mixed, dir_a, IN_MODIFY, "moved"));
  route_check("routing mixed observer close", close(in) == 0);

  in = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  wd = inotify_add_watch(in, moved.c_str(), IN_MODIFY);
  int second = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  int second_wd = inotify_add_watch(second, alias.c_str(), IN_MODIFY);
  route_check("routing independent instances", wd > 0 && second_wd > 0);
  route_check("routing zero write remains zero", write(alias_fd, "", 0) == 0);
  route_check("routing zero write publishes no event", RoutingRead(in, 0).empty() && RoutingRead(second, 0).empty());
  int readonly = open(alias.c_str(), O_RDONLY);
  route_check("routing read-only descriptor", readonly >= 0);
  errno = 0;
  route_check("routing failed write retains error", write(readonly, "j", 1) == -1 && errno == EBADF);
  route_check("routing failed write publishes no event", RoutingRead(in, 0).empty() && RoutingRead(second, 0).empty());
  route_check("routing read-only handle close", close(readonly) == 0);
  route_check("routing one write reaches both observers", write(alias_fd, "k", 1) == 1 &&
              RoutingHas(RoutingRead(in), wd, IN_MODIFY) && RoutingHas(RoutingRead(second), second_wd, IN_MODIFY));
  route_check("routing first final observer close", close(in) == 0);
  route_check("routing second final observer close", close(second) == 0);
  route_check("routing alias handle close", close(alias_fd) == 0);
  route_check("routing original handle close", close(original_fd) == 0);
  route_check("routing unlink replacement", unlink(original.c_str()) == 0);
  route_check("routing unlink original", unlink(moved.c_str()) == 0);
  route_check("routing unlink alias", unlink(alias.c_str()) == 0);
  route_check("routing first directory retired", rmdir(a.c_str()) == 0);
  route_check("routing second directory retired", rmdir(b.c_str()) == 0);
  std::puts("inotify local-write routing: complete");
}
