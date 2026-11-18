"""Bounded local cache of build inputs, never application data or test results.

Archives are immutable, checksummed and published atomically under a file lock.
Restore uses real workspace directories so existing AOSP lifecycle guards keep
their path contract. Compiler output stays in ccache's ordinary local directory.
"""
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile


class CacheError(RuntimeError):
    pass


def digest_file(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def valid_key(key):
    if not isinstance(key, str) or not key or len(key) > 512 or any(ord(c) < 32 for c in key):
        raise CacheError('Invalid cache key')
    return key


class DiskCache:
    def __init__(self, root, workspace, budget=10 * 1024**3, namespace='isolated-fixture'):
        self.root = Path(root).absolute()
        self.workspace = Path(workspace).resolve(strict=True)
        self.budget = budget
        self.namespace = valid_key(namespace)
        self.root.mkdir(parents=True, exist_ok=True)
        if self.root.is_symlink() or self.root.resolve() != self.root:
            raise CacheError('Cache root must be a real directory')

    @contextmanager
    def locked(self):
        with (self.root / 'cache.lock').open('a+b') as lock:
            if sys.platform == 'win32':
                import msvcrt
                lock.seek(0); lock.write(b'0'); lock.flush(); lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_LOCK, 1)
            else:
                import fcntl
                fcntl.flock(lock, fcntl.LOCK_EX)
            try:
                self.cleanup_interrupted()
                yield
            finally:
                if sys.platform == 'win32':
                    lock.seek(0)
                    msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
                else:
                    fcntl.flock(lock, fcntl.LOCK_UN)

    def cleanup_interrupted(self):
        # The process lock excludes live writers. Only our dedicated staging
        # directories are eligible, never a workspace or completed snapshot.
        stages = list(self.root.glob('restore-*'))
        for bucket in self.root.iterdir():
            if re.fullmatch('[0-9a-f]{64}', bucket.name) and bucket.is_dir() and not bucket.is_symlink():
                stages.extend(bucket.glob('save-*'))
        for stage in stages:
            if stage.is_dir() and not stage.is_symlink() and self.root in stage.parents:
                shutil.rmtree(stage)

    def paths(self, names):
        result = []
        for name in names:
            raw = PurePosixPath(name)
            if (not name or raw.is_absolute() or '..' in raw.parts or
                    any(c in name for c in '*?[]\\') or raw == PurePosixPath('.')):
                raise CacheError('Cache paths must be literal workspace children')
            path = self.workspace.joinpath(*raw.parts)
            # Do not follow a workspace symlink to unrelated data on save/restore.
            if path.resolve() != path or self.workspace not in path.parents:
                raise CacheError('Cache path escapes the real workspace')
            result.append(path)
        if not result:
            raise CacheError('At least one cache path is required')
        return result

    def bucket(self, names):
        self.paths(names)
        version = hashlib.sha256(json.dumps([self.namespace, names]).encode()).hexdigest()
        bucket = self.root / version
        bucket.mkdir(exist_ok=True)
        if bucket.is_symlink():
            raise CacheError('Cache bucket cannot be a symlink')
        return bucket

    def entries(self, bucket):
        for entry in bucket.iterdir():
            if re.fullmatch('[0-9a-f]{64}', entry.name) and not entry.is_symlink() and entry.is_dir():
                yield entry

    def restore(self, names, key, prefixes=()):
        valid_key(key)
        for prefix in prefixes:
            valid_key(prefix)
        paths = self.paths(names)
        with self.locked():
            bucket = self.bucket(names)
            candidates = []
            for entry in self.entries(bucket):
                meta = json.loads((entry / 'manifest.json').read_text())
                if (meta['paths'] != names or meta['namespace'] != self.namespace
                        or hashlib.sha256(meta['key'].encode()).hexdigest() != entry.name):
                    raise CacheError('Cache manifest identity mismatch')
                rank = 0 if meta['key'] == key else next(
                    (i + 1 for i, prefix in enumerate(prefixes) if meta['key'].startswith(prefix)), None)
                if rank is not None:
                    candidates.append((rank, -entry.stat().st_mtime, entry, meta))
            if not candidates:
                return ''
            _, _, entry, meta = min(candidates, key=lambda x: (x[0], x[1]))
            payload = entry / 'payload.tar'
            if digest_file(payload) != meta['sha256']:
                raise CacheError('Local cache payload checksum mismatch')
            with tempfile.TemporaryDirectory(prefix='restore-', dir=self.root) as temporary:
                with tarfile.open(payload, 'r:') as archive:
                    for member in archive.getmembers():
                        parts = PurePosixPath(member.name).parts
                        if (not parts or parts[0] not in {f'entry-{i}' for i in range(len(paths))}
                                or '..' in parts or member.name.startswith('/')):
                            raise CacheError('Unexpected cache archive member')
                    archive.extractall(temporary, filter='data')
                extracted = [Path(temporary) / f'entry-{i}' for i in range(len(paths))]
                if any(not p.exists() or p.is_symlink() for p in extracted):
                    raise CacheError('Incomplete cache archive')
                for source, destination in zip(extracted, paths):
                    # Paths were checked before extraction; check again before removal.
                    self.paths([destination.relative_to(self.workspace).as_posix()])
                    if destination.is_dir():
                        shutil.rmtree(destination)
                    elif destination.exists():
                        destination.unlink()
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.move(str(source), destination)
            os.utime(entry, None)
            print(f'LOCAL_CACHE_RESTORE key={meta["key"]} exact={meta["key"] == key}')
            return meta['key']

    def save(self, names, key):
        valid_key(key)
        paths = self.paths(names)
        if any(not path.exists() for path in paths):
            print('LOCAL_CACHE_SKIP missing optional cache path')
            return False
        with self.locked():
            bucket = self.bucket(names)
            entry = bucket / hashlib.sha256(key.encode()).hexdigest()
            if entry.exists():
                print(f'LOCAL_CACHE_EXISTS key={key}')
                return False
            with tempfile.TemporaryDirectory(prefix='save-', dir=bucket) as temporary:
                stage = Path(temporary) / 'entry'
                stage.mkdir()
                payload = stage / 'payload.tar'
                with tarfile.open(payload, 'w:', dereference=False) as archive:
                    for i, path in enumerate(paths):
                        archive.add(path, arcname=f'entry-{i}')
                if payload.stat().st_size > self.budget:
                    print('LOCAL_CACHE_SKIP snapshot exceeds disk budget')
                    return False
                manifest = dict(key=key, namespace=self.namespace, paths=names, sha256=digest_file(payload))
                (stage / 'manifest.json').write_text(json.dumps(manifest), encoding='utf-8')
                os.replace(stage, entry)
            self.prune(entry)
            print(f'LOCAL_CACHE_SAVE key={key} bytes={(entry / "payload.tar").stat().st_size}')
            return True

    def prune(self, keep):
        entries = []
        for bucket in self.root.iterdir():
            if re.fullmatch('[0-9a-f]{64}', bucket.name) and bucket.is_dir() and not bucket.is_symlink():
                entries.extend(self.entries(bucket))
        sizes = {entry: (entry / 'payload.tar').stat().st_size for entry in entries}
        total = sum(sizes.values())
        for entry in sorted(entries, key=lambda p: p.stat().st_mtime):
            if total <= self.budget:
                break
            if entry != keep:
                # Only validated digest-named children of this dedicated cache root.
                if self.root not in entry.parents or entry.is_symlink():
                    raise CacheError('Unsafe cache eviction path')
                shutil.rmtree(entry)
                total -= sizes[entry]


def output(name, value):
    with Path(os.environ['GITHUB_OUTPUT']).open('a', encoding='utf-8') as stream:
        stream.write(f'{name}={value}\n')


def main():
    if (sys.platform != 'linux' or os.environ.get('GITHUB_ACTIONS') != 'true'
            or os.environ.get('RUNNER_ENVIRONMENT') != 'self-hosted'):
        raise CacheError('Local cache is restricted to self-hosted Linux Actions jobs')
    home = Path.home().resolve(strict=True)
    root = home / '.cache' / 'gesher-ci' / 'v1'
    workspace = Path(os.environ['GITHUB_WORKSPACE']).resolve(strict=True)
    namespace = os.environ['CACHE_NAMESPACE']
    if not re.fullmatch(r'refs/(?:pull/[1-9][0-9]*/merge|heads/[A-Za-z0-9_./-]+)', namespace):
        raise CacheError('An explicit repository ref cache boundary is required')
    cache = DiskCache(root, workspace, namespace=namespace)
    phase = os.environ['CACHE_PHASE']
    if phase == 'initialize':
        for tool in ('git', 'curl', 'python3', 'cmake', 'ninja', 'ccache', 'javac'):
            if not shutil.which(tool):
                raise CacheError(f'Missing {tool}; install runner build dependencies first')
        javac = Path(shutil.which('javac')).resolve()
        java_home = javac.parent.parent
        if not (java_home / 'include' / 'jni.h').is_file():
            raise CacheError('JDK JNI headers are missing')
        with Path(os.environ['GITHUB_ENV']).open('a', encoding='utf-8') as stream:
            stream.write(f'BUILD_JOBS=8\nCCACHE_LIMIT=8G\nJAVA_HOME={java_home}\n')
        name = os.environ.get('LLVM_MINGW_NAME', '')
        if name and not re.fullmatch(r'llvm-mingw-[0-9]+-ucrt-ubuntu-[0-9.]+-x86_64', name):
            raise CacheError('Unexpected toolchain directory name')
        compiler = home / 'toolchain' / name / 'bin' / 'x86_64-w64-mingw32-clang'
        hit = bool(name and compiler.is_file() and os.access(compiler, os.X_OK))
        if hit:
            subprocess.run([str(compiler), '--version'], check=True, stdout=subprocess.DEVNULL)
        output('toolchain-hit', str(hit).lower())
        print(f'LOCAL_CACHE_READY root={root} snapshot_budget=10GiB toolchain_present={hit}')
        return
    names = [line.strip() for line in os.environ['CACHE_PATHS'].splitlines() if line.strip()]
    key = valid_key(os.environ['CACHE_KEY'])
    if phase == 'restore':
        prefixes = [line.strip() for line in os.environ.get('CACHE_RESTORE_KEYS', '').splitlines() if line.strip()]
        matched = cache.restore(names, key, prefixes)
        output('cache-primary-key', key)
        output('cache-matched-key', matched)
        output('cache-hit', str(matched == key).lower())
    elif phase == 'save':
        cache.save(names, key)
    else:
        raise CacheError('Unknown cache phase')


if __name__ == '__main__':
    main()
