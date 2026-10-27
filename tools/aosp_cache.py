#!/usr/bin/env python3
"""Guard an isolated GitHub Actions AOSP cache at restore/snapshot boundaries.

Only prepare (immediately after restore, before any source Git child) removes
recognized stale Git locks. Snapshot never removes anything. Neither mode is
allowed on a local checkout or outside GITHUB_WORKSPACE/third_party/aosp.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys


class CacheError(RuntimeError):
    pass


def within(path, root):
    return path == root or root in path.parents


def source_root(path):
    if os.environ.get('GITHUB_ACTIONS') != 'true' or sys.platform != 'linux':
        raise CacheError('AOSP cache cleanup is restricted to Linux GitHub Actions')
    workspace = Path(os.environ['GITHUB_WORKSPACE']).resolve(strict=True)
    expected = workspace / 'third_party' / 'aosp'
    root = Path(path).absolute()
    if root != expected or root.resolve() != expected or not within(root.resolve(), workspace):
        raise CacheError('AOSP cache path must be the isolated workspace/third_party/aosp')
    return root


def git_processes(root):
    """Detect Git commands by actual executable/cwd, including git -C children."""
    found = []
    for proc in Path('/proc').iterdir():
        if not proc.name.isdecimal():
            continue
        known_git = False
        try:
            exe = Path(os.readlink(proc / 'exe')).name
            if not (exe == 'git' or exe.startswith('git-')):
                continue
            known_git = True
            cwd = Path(os.readlink(proc / 'cwd'))
            args = (proc / 'cmdline').read_bytes().split(b'\0')
            paths = [cwd]
            for arg in args[1:]:
                text = os.fsdecode(arg)
                if text and not text.startswith('-'):
                    paths.append((cwd / text).resolve())
            environment = (proc / 'environ').read_bytes().split(b'\0')
            for entry in environment:
                if entry.startswith(b'GIT_DIR=') or entry.startswith(b'GIT_WORK_TREE='):
                    paths.append((cwd / os.fsdecode(entry.split(b'=', 1)[1])).resolve())
            if any(within(path, root) for path in paths):
                found.append({'pid': int(proc.name), 'exe': exe, 'cwd': str(cwd)})
        except (FileNotFoundError, ProcessLookupError):
            continue  # process exited during inspection
        except PermissionError:
            if known_git:
                raise CacheError('cannot inspect live Git process ' + proc.name)
            continue  # another user's unrelated system process executable is hidden
    return found


def locks(root):
    result = []
    if not root.exists():
        return result
    for directory, dirs, files in os.walk(root, followlinks=False):
        if '.git' in files:
            raise CacheError('AOSP cache contains a worktree gitfile instead of an isolated clone')
        if '.git' in dirs:
            gitdir = Path(directory) / '.git'
            if gitdir.is_symlink() or not within(gitdir.resolve(), root):
                raise CacheError('AOSP Git metadata escapes the isolated source workspace')
            dirs.remove('.git')
            for metadata, metadata_dirs, names in os.walk(gitdir, followlinks=False):
                if any((Path(metadata) / name).is_symlink() for name in metadata_dirs):
                    raise CacheError('AOSP Git metadata contains a symlink directory')
                for name in names:
                    if not name.endswith('.lock'):
                        continue
                    lock = Path(metadata) / name
                    if lock.is_symlink() or not within(lock.resolve(), gitdir):
                        raise CacheError('AOSP lock escapes its Git metadata directory')
                    relative = lock.relative_to(gitdir).as_posix()
                    recognized = relative in {
                        'index.lock', 'config.lock', 'shallow.lock', 'HEAD.lock',
                        'packed-refs.lock', 'info/sparse-checkout.lock',
                    } or relative.startswith('refs/')
                    result.append((lock, recognized))
        dirs[:] = [name for name in dirs if not name.startswith('.')]
    return result


def guard(phase, path):
    root = source_root(path)
    active = git_processes(root)
    inventory = locks(root)
    records = []
    for lock, recognized in inventory:
        stat = lock.stat()
        records.append({'path': str(lock.relative_to(root)), 'recognized': recognized,
                        'bytes': stat.st_size, 'mtime_ns': stat.st_mtime_ns,
                        'sha256': hashlib.sha256(lock.read_bytes()).hexdigest()})
    print('AOSP cache boundary ' + json.dumps({'phase': phase, 'active_git': active,
                                              'locks': records}, sort_keys=True), flush=True)
    if active or git_processes(root):
        raise CacheError('active AOSP Git process: refusing cleanup or cache snapshot')
    if phase == 'snapshot':
        if not root.is_dir():
            raise CacheError('missing AOSP workspace: refusing an empty cache snapshot')
        if inventory:
            raise CacheError('Git locks remain: refusing to save a poisoned AOSP cache')
        return
    if phase != 'prepare':
        raise CacheError('unknown AOSP cache phase')
    if any(not recognized for _, recognized in inventory):
        raise CacheError('unrecognized Git lock: refusing automatic deletion')
    for lock, _ in inventory:
        # This boundary runs before any Git source acquisition, never during it.
        lock.unlink()
        print('Removed restored stale Git lock: ' + str(lock.relative_to(root)), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase', choices=('prepare', 'snapshot'))
    parser.add_argument('root')
    args = parser.parse_args()
    try:
        guard(args.phase, args.root)
    except (CacheError, KeyError, OSError) as error:
        print('AOSP cache guard failed: ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
