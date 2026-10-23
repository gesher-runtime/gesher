// Gesher: Windows shell integration of installed Android apps, the Win32 side
// (shell track, docs/installer.md, docs/decisions/0028-shell-start-menu-apps.md):
// Start menu shortcuts with an AppUserModelID, the process's AppUserModelID,
// and the file operations on the Start menu folder (which may be on another
// drive than Gesher's, so not through java.io's drive-less paths). The JNI
// side is shell_natives.cc (com.gesher.app.shell.Shell).
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

}  // namespace shell
}  // namespace gesher
