// Gesher: processes of the Gesher device (services track, docs/services.md,
// docs/decisions/0026-services-persistent-device.md). Win32 only; the JNI
// side is in services_natives.cc (com.gesher.server.Zygote).
#pragma once

#include <stdint.h>

#include <array>
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
int Spawn(const std::u16string& command_line, const std::vector<std::u16string>& set,
          const std::vector<std::u16string>& unset, const std::u16string& log_path,
          bool breakaway);

// An app process's AppContainer (app_container.h, GESHER_SANDBOX=appcontainer):
// the profile name (1..64 of [A-Za-z0-9.-]), its display name and the
// capability names ("internetClient", "gesherRuntimeFiles", ...) its token has.
struct AppContainerSpec {
  std::u16string name;
  std::u16string display_name;
  std::vector<std::u16string> capabilities;
};

// Spawn, never breaking away, in the container `sandbox` (its profile created
// if needed), TEMP and TMP set to the container's own folder.
int SpawnInAppContainer(const std::u16string& command_line,
                        const std::vector<std::u16string>& set,
                        const std::vector<std::u16string>& unset, const std::u16string& log_path,
                        const AppContainerSpec& sandbox);

// Start the broker, service manager and server as one device bootstrap.
// Only the broker and server inherit their respective private control handles.
std::vector<int> StartProtectedDevice(const std::u16string& device,
    const std::vector<std::u16string>& commands, const std::vector<std::u16string>& logs,
    const std::vector<std::u16string>& environment, bool scoped = false);

// A protected SystemServer must enroll the actual contained child before it
// runs. The install UUID and UID come from the server's durable package data.
int SpawnProtectedApp(const std::u16string& command_line, const std::vector<std::u16string>& set,
    const std::vector<std::u16string>& unset, const std::u16string& log_path,
    const AppContainerSpec& sandbox, uint32_t uid, const std::array<uint8_t, 16>& installation,
    uint64_t start_sequence, uint64_t code_pin = 0);
bool ChangeProtectedInstallation(const std::array<uint8_t, 16>& installation, bool revoke);
bool StopProtectedInstallation(const std::array<uint8_t, 16>& installation, uint64_t stop_through);
uint32_t RunContainedStandalone(const std::u16string& command, const AppContainerSpec& spec,
                               const std::u16string& code, uint32_t uid, uint32_t* exit_code);
// A file pinned before code admission remains the same Windows object at spawn.
uint32_t PinStandaloneInput(const std::u16string& code, uint64_t* pin);
void ReleaseStandaloneInput(uint64_t pin);
uint32_t ReadStandaloneInput(uint64_t pin, int64_t offset, void* bytes,
                             uint32_t capacity, uint32_t* copied);
uint32_t RunPinnedContainedStandalone(const std::u16string& command, const AppContainerSpec& spec,
                                      uint64_t pin, uint32_t uid, uint32_t* exit_code);
bool HasProtectedLaunchAuthority(bool scoped = false);
bool PrepareProtectedOperator(int broker_pid, uint64_t birth);

// The container's profile for this Windows user, created if it does not exist.
uint32_t EnsureAppContainer(const std::u16string& name, const std::u16string& display_name);

// Adds an allow ACE for the container `name` (or the capability) to `path`'s
// DACL, inherited by and propagated to its children when `inherit`. 0 or a Win32 error.
uint32_t GrantToAppContainer(const std::u16string& path, const std::u16string& name,
                             uint32_t access, bool inherit);
uint32_t GrantToCapability(const std::u16string& path, const std::u16string& capability,
                           uint32_t access, bool inherit);

// Removes the container's profile (its folder and registry key); 0 when it did not exist.
uint32_t DeleteAppContainer(const std::u16string& name);

// Re-enters this executable/command under `uid` before ART caches its identity.
// Inherits only duplicates of the three standard handles, retains the original
// process handle, and owns the child tree in a kill-on-close job. On success,
// returns 0 and the exact DWORD exit status; otherwise returns a Win32 error.
uint32_t RestartWithUid(int uid, uint32_t* exit_code);

// The process's creation time (a FILETIME), or 0 when no such process runs.
int64_t StartTime(int pid);

// Creation identity from the original retained Spawn handle, or 0 if unavailable.
int64_t ChildStartTime(int pid);

// A spawned process's exit code: 259 (STILL_ACTIVE) while it runs, -1 when
// this process did not start it or does not know.
int ExitCode(int pid);

// Ends the process (TerminateProcess, exit code 137).
bool Kill(int pid);

// Ends only the retained child with this nonzero creation identity. Never opens a PID.
bool KillChild(int pid, int64_t start_time);

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
