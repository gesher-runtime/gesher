# 0097: POSIX FIFOs as WSL FIFO files with a shared ring per FIFO

Status: proposed, 2026-11-25 (PR #256).

## Context

Android applications create named pipes with `mkfifo` and use them between
threads and between the processes of one application. Realm, for example,
creates `<db>.note` and `.realm.temp/realm_<n>.cv` FIFOs, opens them `O_RDWR`
and `O_NONBLOCK`, writes one byte to notify and waits with `poll` or `epoll`;
Bit (`com.bnhp.payments.paymentsapp`) aborted at startup because Gesher's
`mkfifo` failed with `EPERM`.

Linux FIFOs (fifo(7), pipe(7), open(2)) have one pipe object per FIFO,
shared by every process that opens the path: blocking opens wait for the
other end, writes of up to `PIPE_BUF` bytes are atomic, the reader sees end
of file and `POLLHUP` once the last writer closes, the writer `EPIPE` once
the last reader closes, `O_TRUNC` is ignored and the last close discards the
buffered bytes. Windows named pipes are point to point and cannot be opened
again once connected, so they cannot be that object.

## Decision

1. **On disk** a FIFO is an empty file with the `IO_REPARSE_TAG_LX_FIFO`
   reparse tag, as WSL creates them. Setting the tag needs no privilege.
   Windows refuses to follow the tag (`ERROR_CANT_ACCESS_FILE`), so no
   ordinary or truncating open can turn it into a regular file; Gesher's
   open classifies the path before any open that could change it, following
   symbolic links of the last component itself. `stat`, `lstat`, `fstat`,
   `access`, `chmod`, `fchmod`, `realpath`, `utimensat` and `readdir`
   (`DT_FIFO`) report or act on it, unlink and rename work on it unchanged,
   and bionic's `truncate` fails with `EINVAL` without opening it. `mkfifo`
   tags a file under a private name (`.gesher-fifo-<random>`) in the same
   directory and renames it without replacing, so the name appears as a FIFO or not at all
   (`EEXIST` for a racing creator). A volume without reparse points makes
   `mkfifo` fail with `EPERM`, as Linux's vfat does.
2. **The pipe** is a 64 KiB ring in a section named after the FIFO's
   identity (`Local\gesher-fifo-<volume serial><file ID>`), with a named
   mutex and a wake event. Every process of the same session, or of the same
   AppContainer, that opens the path maps the same ring.
3. **Endpoints** are the open file descriptions. A FIFO description's shared
   section is created with a name and listed in the ring's slot table (at
   most 64 open descriptions per FIFO; more fail with `ENFILE`). `dup`
   shares the description and exec inheritance passes its section like any
   other description, so the endpoint stays open while any process holds a
   descriptor for it. A slot whose named section no longer exists belongs
   to descriptors that are all closed or to processes that have exited, and
   is counted out: when a description is destroyed, and otherwise within
   100 ms by a sweep (immediately before a nonblocking read reports
   `EAGAIN`). A named section ends with its last handle, so a launcher passes
   an inheritable reference to the ring's section with each FIFO
   description it exports (`SpawnWithFds`): the buffered bytes survive the
   exporter closing its copies or exiting before the child has bound the
   ring. The child binds its inherited FIFO descriptions while it starts and
   keeps that reference for its lifetime (one handle per exported FIFO
   description). An endpoint that finds its ring recreated registers again.
4. **The descriptor** is of kind `kFifo`: a pipe-type placeholder handle
   without data access, so the descriptor layer treats it as a stream
   (`lseek` and `pread` fail with `ESPIPE`, `fsync` and `ftruncate` with
   `EINVAL`) and `pipe_io.cc` dispatches read, write, `writev`, poll, epoll,
   select and `FIONREAD` to the ring by kind. Status flags live in the shared
   description. A Windows program without Gesher's importer cannot receive a
   FIFO descriptor (`ERROR_NOT_SUPPORTED`); AF_UNIX rights transfer rejects
   one with `EINVAL`.

## Consequences

- Independent opens in different processes rendezvous and exchange bytes;
  Realm's notification sequence works between an application's processes.
- Processes in different namespaces (an AppContainer process and an
  unsandboxed one) do not share a FIFO's ring.
- A peer that exits without closing is noticed within the sweep interval;
  `EPIPE` for a writer whose last reader just exited may come up to 100 ms
  late. Waits look every 10 ms at most, as kernel pipes' polls do.
- Permission bits are modelled as for other files (read-only attribute
  only), without execute bits.
- `SIGPIPE` is not raised on `EPIPE`, as for Gesher's kernel pipes.
- Waits for the ring's mutex end on cancellation, or are bounded (an open
  fails with `EAGAIN` after 5 s, a poll slice reports nothing ready), so a
  peer stopped while holding it cannot wedge other processes indefinitely.
- The last component's symbolic links are walked by Gesher (Windows refuses
  the FIFO's tag at the end of a followed path): absolute and relative
  targets, `.` and `..` resolved lexically against the link's directory as
  Windows does, at most 40 links.
- Known gaps: `fchmod` on a FIFO descriptor reopens the FIFO by the path it
  was opened by and fails with `EOPNOTSUPP` once it was renamed or removed,
  or when that path is longer than 1023 bytes; a process that dies between
  creating and renaming a FIFO leaves its `.gesher-fifo-<random>`
  file behind; `futimens` on a FIFO descriptor fails (the placeholder has no
  times); `/proc/self/fd/<n>` of a FIFO reads `pipe:[...]` rather than the
  path; bionic exports `mkfifo` but not `mknod`/`mknodat`/`mkfifoat`;
  passing a FIFO descriptor over AF_UNIX `SCM_RIGHTS` or Binder fails with
  `EINVAL`.

## Alternatives considered

- **A 16-byte magic marker file** (the first version of #256): every `open`
  and `stat` of any 16-byte file paid an extra read, and truncating opens
  destroyed the marker.
- **One Windows pipe per FIFO shared by handle duplication**: needs a live
  holder process and `PROCESS_DUP_HANDLE` on peers, and a pipe whose writer
  side has closed cannot be joined by a new writer.
- **A broker process**: the extra process and round trips are not needed
  when the kernel's named sections and handle lifetimes already give the
  shared state and the endpoint lifetime.
