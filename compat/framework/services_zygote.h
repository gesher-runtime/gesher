// Gesher: processes of the Gesher device (services track, docs/services.md,
// docs/decisions/0026-services-persistent-device.md). Win32 only; the JNI
// side is in services_natives.cc (com.gesher.server.Zygote).
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

namespace gesher {
namespace zygote {

// Starts `command_line` (CreateProcessW) with no console window and a
// process group of its own, stdin from NUL and stdout and stderr appended
// to `log_path`; it inherits no other handle. The environment is this
// process's with `set` (NAME=value) applied and `unset` (NAME) removed.
// `breakaway` asks to leave this process's jobs, so that it outlives a
// console or job the starter belongs to. Returns the pid, or -(Win32 error).
// The child starts suspended; its actual process handle and birth are retained
// before resuming. Package children require the inherited writer channel and a
// broker registration acknowledgement before resume; generic spawns get no rights.
int Spawn(const std::u16string& command_line, const std::vector<std::u16string>& set,
          const std::vector<std::u16string>& unset, const std::u16string& log_path,
          bool breakaway, int uid = -1, const std::u16string& package = {},
          const std::u16string& install = {});
// Revokes exact owned child instances; does not reopen numeric PIDs.
bool RevokeInstall(const std::u16string& package, const std::u16string& install);
bool IsAuthority();
bool OwnedInstall(int pid, const std::u16string& package, const std::u16string& install);
std::vector<int> StartDevice(const std::vector<std::u16string>& commands,
                            const std::vector<std::u16string>& logs,
                            const std::vector<std::u16string>& env);

// The process's creation time (a FILETIME), or 0 when no such process runs.
// Owned children use the retained CreateProcess instance. Unknown/adopted PIDs
// still use OpenProcess and must not be treated as authenticated launch records.
int64_t StartTime(int pid);

// A spawned process's exit code: 259 (STILL_ACTIVE) while it runs, -1 when
// this process did not start it or does not know.
int ExitCode(int pid);

// Ends the process (TerminateProcess, exit code 137).
bool Kill(int pid);

// Puts the process (0: this one) in this process's job, created on first use
// with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: when this process ends, so does
// everything in the job.
bool JoinJob(int pid);

// A named mutex, held (0 when the wait timed out or failed).
intptr_t Lock(const std::u16string& name, int timeout_ms);
void Unlock(intptr_t handle);

// A named auto-reset event: created (or opened), waited for, set.
intptr_t MakeEvent(const std::u16string& name);
bool WaitEvent(intptr_t handle, int timeout_ms);
bool SignalEvent(const std::u16string& name);
// Connects to the expected broker pid and completes its process handshake.
// No libbinder process state is initialized; timeout and broker exit are bounded.
bool WaitForBinder(const std::u16string& device, int broker_pid, int timeout_ms);

std::u16string CommandLine();
std::u16string ExePath();

}  // namespace zygote
}  // namespace gesher
