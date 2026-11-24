import os
import queue
import subprocess
import tempfile
import threading
import time
from pathlib import Path

import sys
exe = str(Path(sys.argv[1]).resolve())
cases = [
    ('quotes', 'x="hello world"; printf "<%s>\\n" "$x" ""', '<hello world>\n<>\n', 0),
    ('status', 'false; echo $?; true; echo $?', '1\n0\n', 0),
    ('assign-status', 'false; x=hello; echo $?', '0\n', 0),
    ('special-assignment', 'x=old; x=new :; echo "$x"; x=exported export x; echo "$x"; x=evaluated eval \'echo "$x"\'; echo "$x"', 'new\nexported\nevaluated\nevaluated\n', 0),
    ('ordinary-assignment', 'x=old; x=new true; echo "$x"', 'old\n', 0),
    ('repeated-prefix', 'x=old; x=one x=two true; echo "$x"', 'old\n', 0),
    ('special-readonly', 'x=old; x=new readonly x; echo "$x"; x=changed', 'new\n', 2),
    ('if', 'if false; then echo no; elif true; then echo yes; else echo no; fi', 'yes\n', 0),
    ('for', 'for x in 1 2 3; do echo "$x"; done', '1\n2\n3\n', 0),
    ('while', 'i=0; while [ "$i" -lt 3 ]; do i=$((i+1)); echo "$i"; done', '1\n2\n3\n', 0),
    ('function-local', 'x=outer; f() { local x=inner; echo "$x:$1"; return 7; }; f arg; echo "$?:$x"', 'inner:arg\n7:outer\n', 0),
    ('substitution', 'x=$(printf "a\\nb\\n"); printf "[%s]\\n" "$x"', '[a\nb]\n', 0),
    ('substitution-isolation', 'x=outer; y=$(x=inner; echo "$x"); echo "$x:$y"', 'outer:inner\n', 0),
    ('pipeline', 'printf "hello\\n" | cat', 'hello\n', 0),
    ('dd-block-read', 'printf 111122223333 >out; /system/bin/dd if=out bs=4 skip=1 count=1', '2222', 0),
    ('dd-block-update', 'printf 111122223333 >out; printf AAAA | dd of=out bs=4 seek=1 conv=notrunc && cat out', '1111AAAA3333', 0),
    ('dd-block-count-fragment', '{ printf AB; sleep 0.1; printf CD; } | dd bs=4 count=1', 'ABCD', 0),
    ('dd-sync-padding', 'printf AB | dd bs=4 conv=sync status=none', 'AB\x00\x00', 0),
    ('dd-noerror-denied', 'dd if=absent-dd-input conv=noerror,sync status=none', '', 1),
    ('dd-status-progress', 'printf ABCD | dd bs=4 status=progress', 'ABCD', 0),
    ('grep-fields', 'printf "x y z\\na b c\\n" | grep "^x" | awk \'{print $2}\'', 'y\n', 0),
    ('grep-no-match', 'printf "x\\n" | grep z', '', 1),
    ('grep-invalid-regex', 'printf "x\\n" | grep "["', '', 2),
    ('grep-fixed', 'printf "[x]\\nplain\\n" | grep -F "["', '[x]\n', 0),
    ('awk-last-field', 'printf "  a  b c  \\n" | awk \'{print $NF}\'', 'c\n', 0),
    ('awk-unsupported', 'printf x | awk \'BEGIN {exit 0}\'', '', 2),
    ('mount-table-honest', 'grep /dev/block/sdb /proc/mounts | awk \'{print $2}\'', '', 0),
    ('umount-unmounted', 'umount absent-mount-target', '', 1),
    ('dd-invalid-offset', 'dd of=dd-overflow bs=512 seek=18446744073709551615; rc=$?; test -f dd-overflow && exit 99; exit "$rc"', '', 1),
    ('basename', "basename /sys/block/sdb/; /system/bin/basename /data/app/base.apk .apk", 'sdb\nbase\n', 0),
    ('tr-lines', "printf 'card\\nreader\\n' | /system/bin/tr '\\n' ' '", 'card reader ', 0),
    ('tr-ranges', "printf ABCXYZ | tr A-Z a-z", 'abcxyz', 0),
    ('id-effective-uid', "uid=$(id -u) && echo \"$uid\"", None, 0),
    ('guest-virtual-glob', 'for f in /proc/self/stat*; do [ -e "$f" ] && basename "$f"; done', 'stat\nstatm\nstatus\n', 0),
    ('guest-virtual-cat', 'cat /sys/devices/system/cpu/online | tr "\\n" X; echo', None, 0),
    ('pipeline-large', 'i=0; while [ "$i" -lt 9000 ]; do echo 0123456789; i=$((i+1)); done | cat >out; test -s out', '', 0),
    ('pipe-status', 'false | true; echo $?; set -o pipefail; false | true; echo $?', '0\n1\n', 0),
    ('subshell', 'x=outer; (x=inner; echo "$x"); echo "$x"', 'inner\nouter\n', 0),
    ('pid', 'echo "$(echo $$):$$"', None, 0),
    ('redirect', 'echo first > out; echo second >>out; cat <out', 'first\nsecond\n', 0),
    ('dup', 'echo text >&2; echo yes', 'yes\n', 0),
    ('heredoc', 'x=expanded; cat <<EOF\n$x\nEOF\n', 'expanded\n', 0),
    ('quoted-heredoc', "x=expanded; cat <<'EOF'\n$x\nEOF\n", '$x\n', 0),
    ('read', 'printf "one two three\\n" | { read a b; echo "$a:$b"; }', 'one:two three\n', 0),
    ('break', 'for x in 1 2 3; do echo "$x"; break; done', '1\n', 0),
    ('continue', 'for x in 1 2 3; do if [ "$x" = 2 ]; then continue; fi; echo "$x"; done', '1\n3\n', 0),
    ('and-or', 'false && echo no; true || echo no; false || echo yes', 'yes\n', 0),
    ('errexit-condition', 'set -e; false && echo no; echo yes', 'yes\n', 0),
    ('errexit-final', 'set -e; true && false; echo no', '', 1),
    ('case', 'x=hello; case "$x" in h*) echo yes;; *) echo no;; esac', 'yes\n', 0),
    ('positional', 'set -- "hello world" "" last; printf "<%s>\\n" "$@"', '<hello world>\n<>\n<last>\n', 0),
    ('empty-positional', 'set --; printf "%s\\n" "$#"; for x in "$@"; do echo no; done', '0\n', 0),
    ('readonly', 'readonly x=one; x=two', '', 2),
    ('command', 'command echo yes; command -v printf', 'yes\nprintf\n', 0),
    ('exit-trap', 'trap \'echo "done:$?"\' EXIT; exit 7', 'done:7\n', 7),
    ('background', '(echo bg >out) & wait; cat out', 'bg\n', 0),
    ('persistent', 'exec 3>out; echo yes >&3; cat out', 'yes\n', 0),
    ('persistent-group', 'exec 3>&1; exec >global; { echo group; } >group; echo global; cat group global >&3', 'group\nglobal\n', 0),
    ('persistent-scope', 'exec 3>&1; exec >outer; { exec >inner; echo inside; } >group; echo outside; cat inner outer group >&3', 'inside\noutside\n', 0),
    ('persistent-function', 'exec 3>&1; exec >outer; f() { echo function; }; f >inner; echo outside; cat inner outer >&3', 'function\noutside\n', 0),
    ('persistent-pipeline', 'exec 3>&1; exec >outer; echo piped | cat >inner; echo outside; cat inner outer >&3', 'piped\noutside\n', 0),
    ('persistent-extra-fd', 'exec 3>outer; { exec 3>inner; echo inside >&3; } 3>group; echo outside >&3; cat inner outer group', 'inside\noutside\n', 0),
    ('persistent-close-scope', 'exec 3>outer; { exec 3>&-; } 3>inner; echo restored >&3; cat outer', 'restored\n', 0),
    ('long-state', 'x=' + 'a' * 40000 + '; echo "$(echo ${#x})"', '40000\n', 0),
    ('last-status', 'false\n', '', 1),
    ('blank-status', 'false\n\n# comment\n', '', 1),
    ('prefix-positional', 'set -- a "b c" ""; printf "<%s>\\n" "pre$@post"', '<prea>\n<b c>\n<post>\n', 0),
    ('brace-positional', 'set -- a "" c; printf "<%s>\\n" "${@}"', '<a>\n<>\n<c>\n', 0),
    ('getopts', 'set -- -abVALUE -c file; while getopts ab:c opt; do echo "$opt:${OPTARG-}:$OPTIND"; done', 'a::1\nb:VALUE:2\nc::3\n', 0),
    ('getopts-missing', 'set -- -b; getopts :b: opt; echo "$opt:$OPTARG:$OPTIND"', '::b:2\n', 0),
    ('getopts-reset', 'set -- -ab; getopts ab opt; OPTIND=1; getopts ab opt; echo "$opt"', 'a\n', 0),
    ('expanded-glob', 'echo one >one.txt; echo two >two.txt; p="*.txt"; printf "<%s>\\n" $p "$p"', '<one.txt>\n<two.txt>\n<*.txt>\n', 0),
    ('quoted-question-glob', 'echo yes >"rn-q?file"; echo no >rn-qXother; printf "<%s>\\n" "rn-q?"*', '<rn-q?file>\n', 0),
    ('quoted-star-glob', 'echo yes >"rn-s*file"; echo no >rn-sXother; printf "<%s>\\n" "rn-s*"*', '<rn-s*file>\n', 0),
    ('quoted-directory-glob', '[ -d "rn-dir?" ] && [ -e "rn-dir?/one" ] && printf "<%s>\\n" "rn-dir?"/*', '<rn-dir?/one>\n', 0),
    ('wildcard-matches-literal-star', 'echo yes >"rn-leading*more-end"; printf "<%s>\\n" rn-leading*-end', '<rn-leading*more-end>\n', 0),
    ('nounset', 'set -u; echo "${gesher_missing_variable}"; echo no', '', 2),
    ('default-parameter', 'x=; echo "${x-empty}:${x:-empty}:${other-default}:${other:+no}"', ':empty:default:\n', 0),
    ('required-parameter', 'echo "${gesher_missing_variable:?required}"; echo no', '', 2),
    ('trim-pattern', 'x=a.b.c; echo "${x%.*}:${x%%.*}:${x#*.}:${x##*.}"', 'a.b:a:b.c:c\n', 0),
    ('substring', 'x=abcdef; start=2; echo "${x:start:2}"', 'cd\n', 0),
    ('positional-ten', 'set -- 1 2 3 4 5 6 7 8 9 ten; echo "${10}"', 'ten\n', 0),
    # mksh's &> family (lex.c IOBASH): [n]&>file is [n]>file 2>&n, in the
    # foreground; &>>, &>| and &>& likewise. Not "cmd &" then ">file".
    ('and-redirect-both', 'f() { echo out; echo err >&2; }; f &>ar-both; cat ar-both', 'out\nerr\n', 0),
    ('and-redirect-foreground', 'x=old; { x=new; echo hidden; echo hidden >&2; } &>/dev/null; echo "$x"', 'new\n', 0),
    ('and-redirect-stderr-silenced', 'exec 3>&2 2>ar-leak; { echo e >&2; } &>/dev/null; exec 2>&3; cat ar-leak; echo done', 'done\n', 0),
    ('and-redirect-status', 'false &>/dev/null; echo "$?"', '1\n', 0),
    ('and-redirect-append', 'echo one >ar-log; { echo two; echo three >&2; } &>>ar-log; cat ar-log', 'one\ntwo\nthree\n', 0),
    ('and-redirect-clobber', '{ echo a; echo b >&2; } &>|ar-clobber; cat ar-clobber', 'a\nb\n', 0),
    ('and-redirect-dup', 'exec 4>ar-dup; { echo out; echo err >&2; } &>&4; cat ar-dup', 'out\nerr\n', 0),
    ('and-redirect-fd', '{ echo three >&3; echo two >&2; } 3&>ar-fd; cat ar-fd', 'three\ntwo\n', 0),
    ('and-redirect-no-space', 'echo x&>ar-tight; cat ar-tight', 'x\n', 0),
    ('and-and-unchanged', 'true&&echo yes; (echo bg >ar-bg)& wait; cat ar-bg', 'yes\nbg\n', 0),
    # mksh: >&word needs a descriptor; a file name is an error, not &>.
    ('dup-word-error', 'echo hi >&ar-word 2>/dev/null; echo "$?"; test -e ar-word || echo none', '1\nnone\n', 0),
    ('dup-word-message', '{ echo hi >&ar-word2; } 2>&1', None, 1),
    # Without job control an asynchronous list's stdin is /dev/null (mksh
    # jobs.c, POSIX), before its own redirections.
    ('background-stdin', 'echo data >bg-in; { cat & wait; } <bg-in; echo end', 'end\n', 0),
    ('background-stdin-redirect', 'echo data >bg-in2; cat <bg-in2 & wait', 'data\n', 0),
    # toybox which: Gesher's own /system/bin programs first, then $PATH;
    # `su` is not provided (no root): nothing printed, status 1.
    ('which-tools', 'which sh ls which mount; /system/bin/which id', '/system/bin/sh\n/system/bin/ls\n/system/bin/which\n/system/bin/mount\n/system/bin/id\n', 0),
    ('which-usb-tools', 'which dd blockdev grep awk sync umount basename tr; /bin/which /system/bin/dd',
     '/system/bin/dd\n/system/bin/blockdev\n/system/bin/grep\n/system/bin/awk\n/system/bin/sync\n/system/bin/umount\n/system/bin/basename\n/system/bin/tr\n/system/bin/dd\n', 0),
    ('which-su-absent', 'mkdir wh-empty; PATH=$PWD/wh-empty; which su; echo "$?"; which -a su ls; echo "$?"', '1\n/system/bin/ls\n1\n', 0),
    ('which-protected-tools-absent', 'mkdir wh-protected-empty; PATH=$PWD/wh-protected-empty; which su mkfs.vfat fsck.vfat mkfs.exfat fsck.exfat mkfs.ext4 fsck.ext4; echo "$?"', '1\n', 0),
    ('which-path', 'IFS="\n"; mkdir wh-bin wh-bin2; echo x >wh-bin/wh-tool; echo y >wh-bin2/wh-tool; PATH="$PWD/wh-bin:$PWD/wh-bin2"; p=$(which wh-tool); case "$p" in *wh-bin?wh-tool) echo found;; esac; set -- $(which -a wh-tool); echo "$#"; set -- $(which wh-tool wh-tool); echo "$#"; which wh-bin; echo "$?"',
     'found\n2\n2\n1\n', 0),
    ('which-explicit-path', 'mkdir wh-direct wh-shadow wh-shadow/wh-missing; echo x >wh-direct/tool; echo y >wh-shadow/wh-missing/tool; PATH=$PWD/wh-shadow; which wh-direct/tool; which wh-missing/tool; echo "$?"',
     'wh-direct/tool\n1\n', 0),
    ('which-not-in-xbin', '/system/xbin/which su 2>/dev/null; echo "$?"; /bin/which ls', '127\n/system/bin/ls\n', 0),
    ('which-usage', 'which 2>/dev/null; echo "$?"; which -x sh 2>/dev/null; echo "$?"', '1\n1\n', 0),
    # toybox mount as an app: the table is /proc/mounts (Gesher serves none),
    # and mounting fails before mount(2) for a non-root uid.
    ('mount-list', 'mount >mt-out; echo "$?"; while read line; do echo "$line"; done </proc/mounts >mt-proc; test -s mt-out || test -s mt-proc || echo same; mount -t ext4; echo "$?"', '0\nsame\n0\n', 0),
    ('mount-denied', 'mount /dev/block/sda1 /mnt 2>&1; echo "$?"; mount -o remount,rw /system 2>&1; echo "$?"; mount a b c 2>&1; echo "$?"',
     "mount: bad /etc/fstab: No such file or directory\n1\nmount: '/system' not in /proc/mounts\n1\nmount: Max 2 arguments\n1\n", 0),
    ('toybox-dispatch', 'toybox which ls; toybox mount; echo "$?"', '/system/bin/ls\n0\n', 0),
    ('toybox-tools-listed', 'toybox | grep "which mount"',
     'ls cat dd grep awk sync umount blockdev basename tr id ps getprop setprop logcat kill sleep pm am which mount\n', 0),
]
failed = 0
with tempfile.TemporaryDirectory(prefix='gesher-shell-') as folder:
    for label, source, expected, code in cases:
        if label == 'quoted-directory-glob':
            # Set up the same names on disk without depending on host mkdir.
            for name, child in (('rn-dir' + chr(0xF000 + ord('?')), 'one'), ('rn-dirX', 'two')):
                directory = Path(folder, name)
                directory.mkdir()
                (directory / child).touch()
        script = Path(folder, 'case.sh')
        script.write_text(source, encoding='utf-8', newline='\n')
        try:
            result = subprocess.run([exe, '--sh', str(script)], cwd=folder, capture_output=True, timeout=20)
            output = result.stdout.decode('utf-8', errors='replace')
            good = result.returncode == code and (expected is None or output == expected)
            if label == 'pid':
                parts = output.strip().split(':')
                good = good and len(parts) == 2 and parts[0] == parts[1]
            if label == 'id-effective-uid':
                good = good and output.strip().isdigit()
            if label == 'guest-virtual-cat':
                good = good and output.strip().endswith('X') and output[0].isdigit()
            if label == 'dup-word-message':
                good = good and output.endswith(': >&ar-word2: illegal file descriptor name\n')
            print(('PASS  exec3 shell:' if good else 'FAIL  exec3 shell:'), label, '' if good else repr((result.returncode, output[:300], result.stderr[:300])))
            failed += not good
        except subprocess.TimeoutExpired:
            print('FAIL  exec3 shell:', label, 'timeout')
            failed += 1


# Root-check libraries drive `sh` over its stdin and keep that pipe open,
# waiting for marker lines; a hang here is a hung app thread. The shell runs
# with stdin mode (no script argument), stderr merged as the libraries do.
class Session:
    def __init__(self, folder):
        self.process = subprocess.Popen([exe, '--sh'], cwd=folder, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.lines = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for raw in self.process.stdout:
            self.lines.put(raw.decode('utf-8', errors='replace').rstrip('\r\n'))
        self.lines.put(None)

    def write(self, text):
        self.process.stdin.write(text.encode('utf-8'))
        self.process.stdin.flush()

    def until(self, marker, timeout=20):
        seen = []
        deadline = time.monotonic() + timeout
        while True:
            try:
                line = self.lines.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                raise TimeoutError('no ' + repr(marker) + ' after ' + repr(seen))
            if line is None:
                raise EOFError('EOF before ' + repr(marker) + ' after ' + repr(seen))
            if line == marker:
                return seen
            seen.append(line)

    def exit_status(self, timeout=20):
        return self.process.wait(timeout=timeout)

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()


def rootshell(folder):
    # Stericson RootShell (Shell.java, Worker): "echo Started", then the oom_adj
    # lines with &>, then each command followed by "\necho <token> <n> $?\n",
    # then "\nexit 0\n".
    s = Session(folder)
    try:
        s.write('echo Started\n')
        assert s.until('Started') == []
        s.write('(echo -17 > /proc/%d/oom_adj) &> /dev/null\n' % s.process.pid)
        s.write('(echo -17 > /proc/$$/oom_adj) &> /dev/null\n')
        s.write('id\n\necho F*D^W@#FGF 0 $?\n')
        before = s.until('F*D^W@#FGF 0 0')
        assert len(before) == 1 and before[0].startswith('uid='), before
        s.write('ls /system/xbin/su\n\necho F*D^W@#FGF 1 $?\n')
        before = s.until('F*D^W@#FGF 1 1')
        assert len(before) == 1 and before[0].startswith('ls: '), before
        s.write('\nexit 0\n')
        assert s.exit_status() == 0
    finally:
        s.close()


def libsuperuser(folder):
    # Chainfire libsuperuser Shell.run: every command and "exit", stdin still
    # open while it waits for the process.
    s = Session(folder)
    try:
        s.write('echo -BOC-\nid\nexit\n')
        assert s.until('-BOC-') == []
        assert s.lines.get(timeout=20).startswith('uid=')
        assert s.exit_status() == 0
    finally:
        s.close()


def interactive(folder):
    # libsuperuser Shell.Interactive / libsu: a command, then markers on stdout
    # and stderr; ;, &&, || and 2>&1 inside the commands.
    s = Session(folder)
    try:
        s.write('id 2>&1 && echo ok; echo done\necho 3f2a $?\necho 3f2a >&2\n')
        before = s.until('3f2a 0')
        assert len(before) == 3 and before[0].startswith('uid=') and before[1:] == ['ok', 'done'], before
        assert s.until('3f2a') == []
        s.write('which su || echo no-su\necho 3f2b $?\n')
        assert s.until('3f2b 0') == ['no-su']
        s.write('if [ -e /system/xbin/su ]; then\necho rooted\nfi\necho 3f2c $?\n')
        assert s.until('3f2c 0') == []
        s.write('exit\n')
        assert s.exit_status() == 0
    finally:
        s.close()


def eof_status(folder):
    # The status of the last command when stdin ends, as mksh.
    s = Session(folder)
    try:
        s.write('true\nfalse\n')
        s.process.stdin.close()
        assert s.exit_status() == 1
    finally:
        s.close()


sessions = [rootshell, libsuperuser, interactive, eof_status]
with tempfile.TemporaryDirectory(prefix='gesher-shell-stdin-') as folder:
    for check in sessions:
        try:
            check(folder)
            print('PASS  exec3 shell stdin:', check.__name__)
        except Exception as error:  # noqa: BLE001 - report every protocol failure
            print('FAIL  exec3 shell stdin:', check.__name__, repr(error))
            failed += 1
print(f'exec3 shell: {len(cases) + len(sessions)} cases, {failed} failed')
raise SystemExit(bool(failed))
