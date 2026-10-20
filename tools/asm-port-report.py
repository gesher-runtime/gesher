#!/usr/bin/env python3
"""Reports what is left of the x86-64 assembly port (docs/decisions/0006).

  asm-port-report.py <AOSP dir>

Lists every place in ART's x86-64 assembly that still reads or writes
art::Thread through %gs (nterp spells it rSELF), which on Windows is the TEB.
Sites already ported use LOAD_THREAD_SELF / LOAD_THREAD_FIELD /
LOAD_THREAD_ID, or sit in the non-GESHER branch of a GESHER conditional.
"""
import glob
import os
import sys

# Lines that are the original (Linux) side of a ported macro.
PORTED_FORMS = (
    'movq %gs:(REG_VAR',
    r'movq %gs:\offset',
    '%gs:THREAD_SELF_OFFSET, REG_VAR',
    '%gs:THREAD_ID_OFFSET, REG_VAR',
)


def pending_sites(runtime):
    files = sorted(glob.glob(os.path.join(runtime, 'arch/x86_64/*.S')) +
                   glob.glob(os.path.join(runtime, 'interpreter/mterp/x86_64ng/*.S')))
    for path in files:
        depth = []  # stack of (is_gesher_if, in_else)
        for n, line in enumerate(open(path, encoding='utf-8'), 1):
            s = line.strip()
            if s.startswith('#if'):
                depth.append([s.startswith('#if defined(GESHER)'), False])
            elif s.startswith('#else') and depth:
                depth[-1][1] = True
            elif s.startswith('#endif') and depth:
                depth.pop()
            code = line.split('//')[0]
            if '%gs:' not in code and 'rSELF:' not in code:
                continue
            if code.lstrip().startswith('#define') or any(f in code for f in PORTED_FORMS):
                continue
            if any(g and in_else for g, in_else in depth):
                continue  # Linux side of a ported site
            yield os.path.relpath(path, runtime).replace(os.sep, '/'), n, code.strip()


def main():
    runtime = os.path.join(sys.argv[1], 'art', 'runtime')
    sites = list(pending_sites(runtime))
    print('# x86-64 assembly port status\n')
    print(f'**{len(sites)}** thread-register (`%gs`) sites left to port.\n')
    if sites:
        print('```')
        for path, n, code in sites:
            print(f'{path}:{n}: {code}')
        print('```')


if __name__ == '__main__':
    main()
