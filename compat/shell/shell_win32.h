// Gesher: Windows shell integration of installed Android apps, the Win32 side
// (shell track, docs/installer.md, docs/decisions/0028-shell-start-menu-apps.md):
// Start menu shortcuts with an AppUserModelID, the process's AppUserModelID,
// the file operations on the Start menu folder (which may be on another
// drive than Gesher's, so not through java.io's drive-less paths), and the
// apps' URL protocols (their custom schemes, as OAuth redirects come back from
// the browser). The JNI side is shell_natives.cc (com.gesher.app.shell.Shell).
#pragma once

#include <string>

namespace gesher {
namespace shell {

// The user's Start menu Programs folder (FOLDERID_Programs, as Inno Setup's
// {userprograms}); empty when the shell does not answer.
std::u16string ProgramsFolder();

// Creates or replaces the shortcut `link` (IShellLinkW, saved with
// IPersistFile) to `target` with `arguments`, `work_dir`, the icon `icon`
// (index 0; none when empty), `description`, and System.AppUserModel.ID
// `app_id` in its property store (none when empty); then tells the shell the
// item changed. Returns S_OK or the HRESULT that failed.
long CreateShortcut(const std::u16string& link, const std::u16string& target,
                    const std::u16string& arguments, const std::u16string& work_dir,
                    const std::u16string& icon, const std::u16string& description,
                    const std::u16string& app_id);

// SetCurrentProcessExplicitAppUserModelID: the windows of this process group
// under the shortcut with the same ID on the taskbar. Returns its HRESULT.
long SetProcessAppId(const std::u16string& app_id);

// GetCurrentProcessExplicitAppUserModelID; empty when none was set.
std::u16string ProcessAppId();

// Creates `dir` and its missing parents.
bool MakeDirectories(const std::u16string& dir);

// Deletes a file; true when it is gone (or never was there).
bool RemoveFile(const std::u16string& path);

// Removes `dir` if it is empty; true when it is gone.
bool RemoveEmptyDirectory(const std::u16string& dir);

bool Exists(const std::u16string& path);

// Tells the shell the file at `path` changed (SHCNE_UPDATEITEM): Explorer reads
// its icon again.
void NotifyChanged(const std::u16string& path);

// The URL protocol `scheme`, per user (HKCU\Software\Classes\<scheme>: "URL
// Protocol", DefaultIcon `icon` (none when empty) and shell\open\command
// `command`), for the installed package `owner`, whose name it keeps in the
// key's GesherPackage value. A scheme that is not `owner`'s (a key of it in
// HKCU without that value, or in HKLM) is left alone:
// HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS). Returns S_OK or the error.
long RegisterUrlProtocol(const std::u16string& scheme, const std::u16string& owner,
                         const std::u16string& description, const std::u16string& command,
                         const std::u16string& icon);

// Removes the URL protocol `scheme` if it is `owner`'s (RegisterUrlProtocol).
// Returns S_OK when it was removed, S_FALSE when it is not `owner`'s, or the
// error.
long UnregisterUrlProtocol(const std::u16string& scheme, const std::u16string& owner);

// The package whose URL protocol `scheme` is (its GesherPackage value); empty
// when it is none's or not Gesher's.
std::u16string UrlProtocolOwner(const std::u16string& scheme);

}  // namespace shell
}  // namespace gesher
