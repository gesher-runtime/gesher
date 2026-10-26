// gesher-exec's /system/bin/sh (docs/exec.md, ADR 0060): a POSIX shell with
// common mksh syntax, written for Windows (no fork: subshells, pipeline
// stages and command substitutions run serialized state in child processes;
// programs are Windows processes started as execvp would find
// them, exec_spawn.h). Apps reach it through Runtime.exec("sh -c ..."),
// native system()/popen(), and #!/system/bin/sh scripts.
#pragma once

#include <string>
#include <vector>

namespace gesher::exec {

// `sh [-e] [-x] [-c command [name [args...]] | -s [args...] | script [args...]]`:
// argv is what follows "--sh". Returns the shell's exit status. Commands are
// read and run one complete command at a time, so a process that writes
// commands to the shell's stdin gets each one's output before the next.
int RunShell(const std::vector<std::string>& argv);

}  // namespace gesher::exec
