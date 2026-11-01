"""Read-only diagnostic patch audit; never compiles or runs target code."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
UPSTREAM = 'c2fcaa54269f0e773069aeeec8212cac71f417483bf65d5e75f100311df4080b'
MINIMUM = 'c8fd0dece0f412bbdffab2df27df462eeef131a6b671e0e5a13ccde2364d0f78'
DIAGNOSTIC = 'f90a3275d5713751b3691dc4866831c746390e1784357a42285645d653ea71bf'

def digest(raw):
    return hashlib.sha256(raw).hexdigest()

def main():
    p = argparse.ArgumentParser()
    p.add_argument('upstream_thread', type=Path)
    args = p.parse_args()
    upstream = args.upstream_thread.read_bytes()
    assert digest(upstream) == UPSTREAM
    with tempfile.TemporaryDirectory(prefix='minimum-join-source-') as tmp:
        work = Path(tmp)
        target = work / 'mingw-w64-libraries/winpthreads/src/thread.c'
        target.parent.mkdir(parents=True)
        target.write_bytes(upstream)
        patches = ROOT / 'tools/host-pthread-authority'
        for name in ('minimum-detached-retirement.patch', 'minimum-noattach-join.patch'):
            subprocess.run(['git', 'apply', str(patches / name)], cwd=work, check=True)
        minimum = target.read_bytes().replace(b'\r\n', b'\n')
        assert digest(minimum) == MINIMUM
        subprocess.run(['git', 'apply', str(patches / 'minimum-join-wait-diagnostic.patch')], cwd=work, check=True)
        diagnostic = target.read_bytes().replace(b'\r\n', b'\n')
        assert digest(diagnostic) == DIAGNOSTIC
        start = diagnostic.index(b'#ifdef GESHER_MINIMUM_JOIN_WAIT_DIAGNOSTIC\n')
        end = diagnostic.index(b'\nint\npthread_join (pthread_t t, void **res)', start)
        reduced = diagnostic[:start] + diagnostic[end + 1:]
        block_start = reduced.index(b'    {\n#ifdef GESHER_MINIMUM_JOIN_WAIT_DIAGNOSTIC\n')
        expected_block = b"    {\n#ifdef GESHER_MINIMUM_JOIN_WAIT_DIAGNOSTIC\n      if (t == gesher_diag_target)\n        gesher_minimum_join_diag_wait (tv->h);\n      else\n#endif\n        WaitForSingleObject (tv->h, INFINITE);\n    }\n"
        block_end = block_start + len(expected_block)
        assert reduced[block_start:block_end] == expected_block
        reduced = reduced[:block_start] + b'    WaitForSingleObject (tv->h, INFINITE);\n' + reduced[block_end:]
        assert reduced == minimum
    print('SOURCE_STACK_PASS pinned_upstream=1 minimum=1 diagnostic=1 exact_macro_off_projection=1 native_execution=0')

if __name__ == '__main__':
    main()
