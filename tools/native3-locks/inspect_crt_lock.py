"""CI-only: show actual loaded CRT named lock exports and their original bytes.

No private FILE layout/critical-section offset or lock-owner verdict is assumed.
"""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--objdump', required=True, type=Path)
    args = parser.parse_args()
    assert os.environ.get('GITHUB_ACTIONS') == 'true' and os.name == 'nt', 'Windows CI only'
    assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip() == os.environ['GITHUB_SHA']
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.GetSystemDirectoryW.argtypes = [wintypes.LPWSTR, wintypes.UINT]
    kernel.GetSystemDirectoryW.restype = wintypes.UINT
    kernel.GetModuleHandleExW.argtypes = [wintypes.DWORD, ctypes.c_void_p, ctypes.POINTER(wintypes.HMODULE)]
    kernel.GetModuleHandleExW.restype = wintypes.BOOL
    kernel.GetModuleFileNameW.argtypes = [wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD]
    kernel.GetModuleFileNameW.restype = wintypes.DWORD
    kernel.GetCurrentProcess.argtypes = []
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    kernel.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                       ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
    kernel.ReadProcessMemory.restype = wintypes.BOOL
    system = ctypes.create_unicode_buffer(32768)
    length = kernel.GetSystemDirectoryW(system, len(system))
    assert 0 < length < len(system)
    path = Path(system.value) / 'ucrtbase.dll'
    crt = ctypes.CDLL(str(path))  # Retained throughout export resolution/read/disassembly.
    data = path.read_bytes()
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    assert data[pe:pe + 4] == b'PE\0\0'
    assert struct.unpack_from('<H', data, pe + 4)[0] == 0x8664
    assert struct.unpack_from('<H', data, pe + 24)[0] == 0x20b
    image_base = struct.unpack_from('<Q', data, pe + 24 + 24)[0]
    sections = struct.unpack_from('<H', data, pe + 6)[0]
    optional_size = struct.unpack_from('<H', data, pe + 20)[0]
    section_table = pe + 24 + optional_size

    def original_bytes(rva, count):
        found = []
        for index in range(sections):
            section = section_table + 40 * index
            virtual, raw_size, raw = struct.unpack_from('<III', data, section + 12)
            if virtual <= rva and rva + count <= virtual + raw_size:
                offset = raw + rva - virtual
                assert offset + count <= len(data)
                found.append(data[offset:offset + count])
        assert len(found) == 1, 'Export is not uniquely file-backed'
        return found[0]

    print(json.dumps(dict(kind='ACTUAL_CRT_IMPLEMENTATION_SOURCE', source=os.environ['GITHUB_SHA'],
                          run=os.environ['GITHUB_RUN_ID'], attempt=os.environ['GITHUB_RUN_ATTEMPT'],
                          module=str(path), module_sha256=hashlib.sha256(data).hexdigest(),
                          analysis_tool_sha256=hashlib.sha256(args.objdump.read_bytes()).hexdigest())), flush=True)
    subprocess.run([str(args.objdump), '--version'], check=True)
    for name in ('_lock_file', '_unlock_file'):
        function = getattr(crt, name)
        address = ctypes.cast(function, ctypes.c_void_p).value
        assert address
        module = wintypes.HMODULE()
        assert kernel.GetModuleHandleExW(0x00000004 | 0x00000002, address, ctypes.byref(module)), ctypes.get_last_error()
        actual = ctypes.create_unicode_buffer(32768)
        length = kernel.GetModuleFileNameW(module, actual, len(actual))
        assert 0 < length < len(actual) and Path(actual.value).resolve() == path.resolve()
        rva = address - module.value
        original = original_bytes(rva, 128)
        loaded = (ctypes.c_ubyte * len(original))()
        copied = ctypes.c_size_t()
        assert kernel.ReadProcessMemory(kernel.GetCurrentProcess(), address, loaded,
                                        len(loaded), ctypes.byref(copied)), ctypes.get_last_error()
        assert copied.value == len(loaded) and bytes(loaded) == original, 'Loaded export bytes differ from exact file'
        print(json.dumps(dict(kind='ACTUAL_CRT_NAMED_EXPORT', name=name, module_base=hex(module.value),
                              address=hex(address), rva=hex(rva), original_bytes=original.hex(),
                              loaded_bytes_equal=True)), flush=True)
        with tempfile.TemporaryDirectory(prefix='native3-crt-code-') as temporary:
            snapshot = Path(temporary) / 'ucrtbase.dll'
            snapshot.write_bytes(data)
            assert hashlib.sha256(snapshot.read_bytes()).digest() == hashlib.sha256(data).digest()
            print('ACTUAL_CRT_DISASSEMBLY_SNAPSHOT sha256=' + hashlib.sha256(data).hexdigest(), flush=True)
            subprocess.run([str(args.objdump), '--disassemble', '--no-show-raw-insn',
                            '--start-address=' + hex(image_base + rva),
                            '--stop-address=' + hex(image_base + rva + len(original)), str(snapshot)], check=True)
    print('ACTUAL_CRT_IMPLEMENTATION_COMPLETE exports=2 private_layout_assumed=false lock_owner_proof=false runtime_acceptance=false', flush=True)


if __name__ == '__main__':
    main()
