"""Test-only, confined failure evidence; never changes benchmark acceptance."""
from contextlib import ExitStack
from itertools import islice
import os
from pathlib import Path
import re
import stat
import subprocess
import sys

LIMIT = 32768
TIMEOUT_SECONDS = 3


def _excerpt(fd):
    data = os.read(fd, 2049)
    if len(data) > 2048:
        size = os.lseek(fd, 0, os.SEEK_END)
        os.lseek(fd, -1024, os.SEEK_END)
        data = data[:1024] + f'\n... {size - 2048} bytes omitted ...\n'.encode() + os.read(fd, 1024)
    return data.decode('utf-8', errors='replace')


def _names(directory):
    # Bound enumeration before sorting or selecting; do not stat arbitrary entries.
    with os.scandir(directory) as entries:
        return sorted(entry.name for entry in islice(entries, 64)
                      if re.fullmatch(r'[0-9]{5}-[A-Za-z0-9_-]{1,100}', entry.name))[:4]


def _windows_root_child(parent, canonical):
    # Handle normalization can expand an 8.3 component (RUNNER~1 -> runneradmin).
    # The opened object must still be an immediate child of the checked parent;
    # a normalized path in another parent/volume is never an acceptable alias.
    actual_parent, separator, name = canonical.rpartition('\\')
    if not separator or name in ('', '.', '..') or actual_parent.casefold() != parent.casefold():
        raise OSError('Directory location changed')
    return canonical


class _PosixDirectory:
    flags = os.O_RDONLY | getattr(os, 'O_DIRECTORY', 0) | getattr(os, 'O_NOFOLLOW', 0)

    def __init__(self, fd):
        self.fd = fd

    @classmethod
    def root(cls, path):
        if not all(hasattr(os, name) for name in ('O_DIRECTORY', 'O_NOFOLLOW', 'O_PATH')):
            raise OSError('Confined diagnostics require Linux descriptor-relative opens')
        fd = os.open(path.anchor, cls.flags)
        try:
            for part in path.parts[1:]:
                child = os.open(part, cls.flags, dir_fd=fd)
                os.close(fd)
                fd = child
            return cls(fd)
        except BaseException:
            os.close(fd)
            raise

    def __enter__(self):
        return self

    def __exit__(self, *_):
        os.close(self.fd)

    def names(self):
        return _names(self.fd)

    def directory(self, name):
        return type(self)(os.open(name, self.flags, dir_fd=self.fd))

    def read(self, name):
        # O_PATH never opens a FIFO/device for data access. Inspect that exact
        # object, then reopen its held descriptor, not its replaceable pathname.
        with ExitStack() as cleanup:
            metadata = os.open(name, os.O_PATH | os.O_NOFOLLOW, dir_fd=self.fd)
            cleanup.callback(os.close, metadata)
            info = os.fstat(metadata)
            if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
                raise OSError('Unsafe evidence: expected a single-link regular file')
            fd = os.open(f'/proc/self/fd/{metadata}', os.O_RDONLY | os.O_NONBLOCK)
            cleanup.callback(os.close, fd)
            opened = os.fstat(fd)
            if (not stat.S_ISREG(opened.st_mode) or opened.st_nlink != 1 or
                    (opened.st_dev, opened.st_ino) != (info.st_dev, info.st_ino)):
                raise OSError('Evidence identity changed')
            return _excerpt(fd)


class _WindowsDirectory:
    """Hold checked ancestors without write/delete sharing until reads finish.

    CreateFileW OPEN_REPARSE_POINT opens the link itself; handle metadata rejects
    it before any read. READ access plus deny-write/delete sharing pins each
    checked ancestor until its data reads finish.
    https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-createfilew
    """
    def __init__(self, api, handle, path, cleanup):
        self.api, self.handle, self.path, self.cleanup = api, handle, path, cleanup

    @classmethod
    def root(cls, path):
        import ctypes as c
        from ctypes import wintypes as w
        if not re.fullmatch(r'[A-Za-z]:', path.drive):
            raise OSError('Diagnostic output must be on a local drive')
        class FileInfo(c.Structure):
            _fields_ = [('attributes', w.DWORD), ('times', w.DWORD * 6), ('volume', w.DWORD),
                        ('size_high', w.DWORD), ('size_low', w.DWORD), ('links', w.DWORD),
                        ('index_high', w.DWORD), ('index_low', w.DWORD)]
        k = c.WinDLL('kernel32', use_last_error=True)
        signatures = {
            'CreateFileW': ([w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p, w.DWORD, w.DWORD, w.HANDLE], w.HANDLE),
            'GetFileType': ([w.HANDLE], w.DWORD),
            'GetFileInformationByHandle': ([w.HANDLE, c.POINTER(FileInfo)], w.BOOL),
            'GetFinalPathNameByHandleW': ([w.HANDLE, w.LPWSTR, w.DWORD, w.DWORD], w.DWORD),
            'CloseHandle': ([w.HANDLE], w.BOOL),
        }
        for name, signature in signatures.items():
            getattr(k, name).argtypes, getattr(k, name).restype = signature

        def checked(handle, directory):
            if handle == c.c_void_p(-1).value or not handle:
                raise c.WinError(c.get_last_error())
            info = FileInfo()
            if k.GetFileType(handle) != 1 or not k.GetFileInformationByHandle(handle, c.byref(info)):
                raise OSError('Unsafe evidence: not a metadata-verifiable disk object')
            # Reparse points, device/offline files and directories are never data.
            if (info.attributes & (0x400 | 0x40 | 0x1000) or
                    bool(info.attributes & 0x10) != directory or (not directory and info.links != 1)):
                raise OSError('Unsafe evidence: reparse, special, or multiply-linked object')

        def opened(name, directory):
            handle = k.CreateFileW(name, 0x80000000, 1, None, 3, 0x00200000 | 0x02000000 | 0x00100000, None)
            try:
                checked(handle, directory)
                buffer = c.create_unicode_buffer(32768)
                count = k.GetFinalPathNameByHandleW(handle, buffer, len(buffer), 1)  # Volume GUID; no UNC fallback.
                if not 0 < count < len(buffer):
                    raise OSError('Unable to establish canonical evidence location')
                return handle, buffer.value.rstrip('\\')
            except BaseException:
                if handle and handle != c.c_void_p(-1).value:
                    k.CloseHandle(handle)
                raise

        api = (c, k, checked, opened)
        cleanup = ExitStack()
        try:
            handle, canonical = opened(path.anchor, True)
            cleanup.callback(k.CloseHandle, handle)
            for part in path.parts[1:]:
                parent = canonical
                handle, canonical = opened(parent + '\\' + part, True)
                cleanup.callback(k.CloseHandle, handle)
                canonical = _windows_root_child(parent, canonical)
            return cls(api, handle, canonical, cleanup)
        except BaseException:
            cleanup.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.cleanup.close()

    def names(self):
        return _names(self.path)

    def directory(self, name):
        _, k, _, opened = self.api
        handle, canonical = opened(self.path + '\\' + name, True)
        cleanup = ExitStack()
        cleanup.callback(k.CloseHandle, handle)
        if canonical.casefold() != (self.path + '\\' + name).casefold():
            cleanup.close()
            raise OSError('Attempt directory location changed')
        return type(self)(self.api, handle, canonical, cleanup)

    def read(self, name):
        import msvcrt
        _, k, _, opened = self.api
        handle, canonical = opened(self.path + '\\' + name, False)
        try:
            if canonical.casefold() != (self.path + '\\' + name).casefold():
                raise OSError('Evidence location changed')
            fd = msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY)
        except BaseException:
            k.CloseHandle(handle)
            raise
        try:
            return _excerpt(fd)
        finally:
            os.close(fd)  # CRT descriptor owns the verified native handle.


def _collect(output):
    path = Path(output)
    if '..' in path.parts or len(str(path)) > 4096:
        raise OSError('Unsafe diagnostic root')
    path = path.absolute()
    if len(path.parts) > 64:
        raise OSError('Diagnostic root has too many components')
    parts = ['Runner failure evidence: at most 64 directory entries, four attempts, bounded file excerpts.']
    backend = _WindowsDirectory if os.name == 'nt' else _PosixDirectory

    def append(directory, name, prefix=''):
        try:
            excerpt = directory.read(name)
        except OSError as error:
            excerpt = f'<unavailable: {type(error).__name__}: {error}>'
        parts.append(f'--- {prefix}{name} ---\n{excerpt}')

    with backend.root(path) as root:
        for name in ('runs.json', 'summary.json'):
            append(root, name)
        for name in root.names():
            try:
                with root.directory(name) as attempt:
                    for filename in ('attempt.json', 'stderr.log', 'stdout.log', 'phase.json', 'ack.json', 'result.json'):
                        append(attempt, filename, name + '/')
            except OSError:
                parts.append('<unsafe or unavailable attempt directory omitted>')
    details = '\n'.join(parts)
    marker = '\n... remaining diagnostic output omitted ...'
    return details if len(details) <= LIMIT else details[:LIMIT - len(marker)] + marker


# Capture trusted module bytes before any workload runs. The isolated worker
# never reloads a repository file that a failed/mutating fixture could replace.
_WORKER_SOURCE = Path(__file__).read_text(encoding='utf-8') if '__file__' in globals() else None


def runner_failure_diagnostics(output):
    try:
        result = subprocess.run([sys.executable, '-I', '-c', _WORKER_SOURCE, str(output)],
                                capture_output=True, timeout=TIMEOUT_SECONDS, check=False)
        if result.returncode:
            return f'<diagnostics unavailable: worker exit {result.returncode}>'
        return result.stdout.decode('utf-8', errors='replace')[:LIMIT]
    except (OSError, ValueError, TypeError, subprocess.SubprocessError) as error:
        # subprocess.run kills and reaps its sole worker on timeout.
        return f'<diagnostics unavailable: {type(error).__name__}>'


if __name__ == '__main__':
    try:
        details = _collect(sys.argv[1])
    except OSError as error:
        details = f'<diagnostics unavailable: {type(error).__name__}: {error}>'
    sys.stdout.buffer.write(details.encode('utf-8', errors='replace')[:LIMIT * 4])
