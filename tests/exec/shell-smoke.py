import os
import subprocess
import tempfile
from pathlib import Path

import sys
exe = str(Path(sys.argv[1]).resolve())
cases = [
    ('quotes', 'x="hello world"; printf "<%s>\\n" "$x" ""', '<hello world>\n<>\n', 0),
    ('status', 'false; echo $?; true; echo $?', '1\n0\n', 0),
    ('assign-status', 'false; x=hello; echo $?', '0\n', 0),
    ('if', 'if false; then echo no; elif true; then echo yes; else echo no; fi', 'yes\n', 0),
    ('for', 'for x in 1 2 3; do echo "$x"; done', '1\n2\n3\n', 0),
    ('while', 'i=0; while [ "$i" -lt 3 ]; do i=$((i+1)); echo "$i"; done', '1\n2\n3\n', 0),
    ('function-local', 'x=outer; f() { local x=inner; echo "$x:$1"; return 7; }; f arg; echo "$?:$x"', 'inner:arg\n7:outer\n', 0),
    ('substitution', 'x=$(printf "a\\nb\\n"); printf "[%s]\\n" "$x"', '[a\nb]\n', 0),
    ('substitution-isolation', 'x=outer; y=$(x=inner; echo "$x"); echo "$x:$y"', 'outer:inner\n', 0),
    ('pipeline', 'printf "hello\\n" | cat', 'hello\n', 0),
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
    ('long-state', 'x=' + 'a' * 40000 + '; echo "$(echo ${#x})"', '40000\n', 0),
    ('last-status', 'false\n', '', 1),
    ('blank-status', 'false\n\n# comment\n', '', 1),
    ('prefix-positional', 'set -- a "b c" ""; printf "<%s>\\n" "pre$@post"', '<prea>\n<b c>\n<post>\n', 0),
    ('brace-positional', 'set -- a "" c; printf "<%s>\\n" "${@}"', '<a>\n<>\n<c>\n', 0),
    ('getopts', 'set -- -abVALUE -c file; while getopts ab:c opt; do echo "$opt:${OPTARG-}:$OPTIND"; done', 'a::1\nb:VALUE:2\nc::3\n', 0),
    ('getopts-missing', 'set -- -b; getopts :b: opt; echo "$opt:$OPTARG:$OPTIND"', '::b:2\n', 0),
    ('getopts-reset', 'set -- -ab; getopts ab opt; OPTIND=1; getopts ab opt; echo "$opt"', 'a\n', 0),
    ('expanded-glob', 'echo one >one.txt; echo two >two.txt; p="*.txt"; printf "<%s>\\n" $p "$p"', '<one.txt>\n<two.txt>\n<*.txt>\n', 0),
    ('nounset', 'set -u; echo "${gesher_missing_variable}"; echo no', '', 2),
    ('default-parameter', 'x=; echo "${x-empty}:${x:-empty}:${other-default}:${other:+no}"', ':empty:default:\n', 0),
    ('required-parameter', 'echo "${gesher_missing_variable:?required}"; echo no', '', 2),
    ('trim-pattern', 'x=a.b.c; echo "${x%.*}:${x%%.*}:${x#*.}:${x##*.}"', 'a.b:a:b.c:c\n', 0),
    ('substring', 'x=abcdef; start=2; echo "${x:start:2}"', 'cd\n', 0),
    ('positional-ten', 'set -- 1 2 3 4 5 6 7 8 9 ten; echo "${10}"', 'ten\n', 0),
]
failed = 0
with tempfile.TemporaryDirectory(prefix='gesher-shell-') as folder:
    for label, source, expected, code in cases:
        script = Path(folder, 'case.sh')
        script.write_text(source, encoding='utf-8', newline='\n')
        try:
            result = subprocess.run([exe, '--sh', str(script)], cwd=folder, capture_output=True, timeout=20)
            output = result.stdout.decode('utf-8', errors='replace')
            good = result.returncode == code and (expected is None or output == expected)
            if label == 'pid':
                parts = output.strip().split(':')
                good = good and len(parts) == 2 and parts[0] == parts[1]
            print(('PASS  exec3 shell:' if good else 'FAIL  exec3 shell:'), label, '' if good else repr((result.returncode, output[:300], result.stderr[:300])))
            failed += not good
        except subprocess.TimeoutExpired:
            print('FAIL  exec3 shell:', label, 'timeout')
            failed += 1
print(f'exec3 shell: {len(cases)} cases, {failed} failed')
raise SystemExit(bool(failed))
