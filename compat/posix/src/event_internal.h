// gesher-base: eventfd and epoll fds (epoll.cc), and the poll loop they
// share with poll() (fd.cc).
//
// Like a socket's, an eventfd's or epoll's fd is a CRT fd open on NUL that
// epoll.cc maps to its object. Duplicates share the object, as they share
// the open file description on Linux.
#pragma once

#include <poll.h>
#include <windows.h>

#include <memory>

namespace gesher {

struct EventFd;

// Null if fd is not an eventfd.
std::shared_ptr<EventFd> LookupEventFd(int fd);
// The poll events of `events` that are ready. When none are and `wake` is
// not null, *wake is a Win32 event that is signaled once one may be (null
// if none can be).
short PollEventFd(EventFd& eventfd, short events, HANDLE* wake);
bool IsNonBlocking(EventFd& eventfd);
void SetNonBlocking(EventFd& eventfd, bool on);

// The fd.cc overrides call these for every fd; each returns false, doing
// nothing, if fd is neither an eventfd nor an epoll fd.
bool ReadEventObject(int fd, void* buf, unsigned int count, int* result);
bool WriteEventObject(int fd, const void* buf, unsigned int count, int* result);
// Drops fd's reference to its object; the caller closes the CRT fd.
bool ForgetEventObject(int fd);
// After `fd2` was made a CRT duplicate of `fd`: fd2 shares fd's object.
void DupEventObject(int fd, int fd2);

// Takes a closed (or replaced) fd number out of every epoll interest list.
void ForgetEpollEntries(int fd);

// poll() (fd.cc), which also returns, with what is ready so far and
// *interrupted set, when the auto-reset event `interrupt` is signaled.
// `interrupt` may be null.
int PollFds(pollfd* fds, nfds_t nfds, int timeout, HANDLE interrupt, bool* interrupted);

}  // namespace gesher
