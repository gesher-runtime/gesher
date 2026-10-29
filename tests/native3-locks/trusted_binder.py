"""Win64 trusted host fixture bootstrap, using the production private ABI3.

Not a guest sandbox: this controller is outside untrusted AppContainers.
No public broker mode or caller-supplied PID can create an admission record.
"""
import ctypes as C
from ctypes import wintypes as W
import os
import subprocess
import time
import uuid


class Context(C.Structure):
    _fields_ = [('crt', W.DWORD), ('magic', W.DWORD), ('version', W.DWORD), ('uid', W.DWORD),
                ('control', C.c_uint64), ('broker', C.c_uint64), ('kind', W.DWORD), ('server', W.DWORD)]


class Request(C.Structure):
    _fields_ = [('magic', W.DWORD), ('version', W.DWORD), ('op', W.DWORD), ('uid', W.DWORD),
                ('process', C.c_uint64), ('birth', C.c_uint64), ('generation', C.c_ubyte * 16),
                ('package', C.c_char * 256), ('restricted', W.DWORD), ('endpoint', C.c_ubyte * 16)]


class Startup(C.Structure):
    _fields_ = [('cb', W.DWORD), ('reserved', W.LPWSTR), ('desktop', W.LPWSTR), ('title', W.LPWSTR),
                ('x', W.DWORD), ('y', W.DWORD), ('width', W.DWORD), ('height', W.DWORD),
                ('charsx', W.DWORD), ('charsy', W.DWORD), ('fill', W.DWORD), ('flags', W.DWORD),
                ('show', W.WORD), ('size', W.WORD), ('data', C.c_void_p),
                ('input', W.HANDLE), ('output', W.HANDLE), ('error', W.HANDLE), ('attributes', C.c_void_p)]


class ProcessInfo(C.Structure):
    _fields_ = [('process', W.HANDLE), ('thread', W.HANDLE), ('pid', W.DWORD), ('tid', W.DWORD)]


class Child:
    def __init__(self, owner, info, output):
        self.owner, self._handle, self.pid, self.stdout = owner, info.process, info.pid, output
        self.returncode = None

    def poll(self):
        if self.owner.api.WaitForSingleObject(self._handle, 0) == 0:
            code = W.DWORD()
            self.owner.check(self.owner.api.GetExitCodeProcess(self._handle, C.byref(code)))
            self.returncode = code.value
        return self.returncode

    def wait(self, timeout=None):
        if self.returncode is not None:
            return self.returncode
        if self.owner.api.WaitForSingleObject(self._handle, int(timeout * 1000) if timeout is not None else 0xffffffff) != 0:
            raise subprocess.TimeoutExpired(str(self.pid), timeout)
        return self.poll()

    def communicate(self, timeout):
        self.wait(timeout)
        self.stdout.flush()
        self.stdout.seek(0)
        # Preserve Popen(text=True)'s universal-newline contract. The inherited
        # native CRT stream writes CRLF even though the host capture is binary;
        # returning raw CR would change the original exact-line assertions.
        text = self.stdout.read().decode(errors='replace')
        return text.replace('\r\n', '\n').replace('\r', '\n'), None

    def kill(self):
        self.owner.check(self.owner.api.TerminateProcess(self._handle, 211))


class Bootstrap:
    def __init__(self, job):
        self.job, self.children = job, []
        self.api = C.WinDLL('kernel32', use_last_error=True)
        declarations = {
            'CreateNamedPipeW': (W.HANDLE, [W.LPCWSTR, W.DWORD, W.DWORD, W.DWORD, W.DWORD, W.DWORD, W.DWORD, C.c_void_p]),
            'CreateFileW': (W.HANDLE, [W.LPCWSTR, W.DWORD, W.DWORD, C.c_void_p, W.DWORD, W.DWORD, W.HANDLE]),
            'ConnectNamedPipe': (W.BOOL, [W.HANDLE, C.c_void_p]),
            'SetNamedPipeHandleState': (W.BOOL, [W.HANDLE, C.c_void_p, C.c_void_p, C.c_void_p]),
            'SetHandleInformation': (W.BOOL, [W.HANDLE, W.DWORD, W.DWORD]),
            'InitializeProcThreadAttributeList': (W.BOOL, [C.c_void_p, W.DWORD, W.DWORD, C.c_void_p]),
            'UpdateProcThreadAttribute': (W.BOOL, [C.c_void_p, W.DWORD, C.c_size_t, C.c_void_p, C.c_size_t, C.c_void_p, C.c_void_p]),
            'DeleteProcThreadAttributeList': (None, [C.c_void_p]),
            'CreateProcessW': (W.BOOL, [W.LPCWSTR, W.LPWSTR, C.c_void_p, C.c_void_p, W.BOOL, W.DWORD, C.c_void_p, W.LPCWSTR, C.c_void_p, C.c_void_p]),
            'GetCurrentProcess': (W.HANDLE, []),
            'DuplicateHandle': (W.BOOL, [W.HANDLE, W.HANDLE, W.HANDLE, C.c_void_p, W.DWORD, W.BOOL, W.DWORD]),
            'GetProcessTimes': (W.BOOL, [W.HANDLE, C.c_void_p, C.c_void_p, C.c_void_p, C.c_void_p]),
            'ReadFile': (W.BOOL, [W.HANDLE, C.c_void_p, W.DWORD, C.c_void_p, C.c_void_p]),
            'WriteFile': (W.BOOL, [W.HANDLE, C.c_void_p, W.DWORD, C.c_void_p, C.c_void_p]),
            'ResumeThread': (W.DWORD, [W.HANDLE]),
            'WaitForSingleObject': (W.DWORD, [W.HANDLE, W.DWORD]),
            'GetExitCodeProcess': (W.BOOL, [W.HANDLE, C.c_void_p]),
            'TerminateProcess': (W.BOOL, [W.HANDLE, W.UINT]),
            'CloseHandle': (W.BOOL, [W.HANDLE]),
        }
        for name, (result, args) in declarations.items():
            fn = getattr(self.api, name)
            fn.restype, fn.argtypes = result, args
        assert C.sizeof(Context) == 40 and C.sizeof(Request) == 328
        self.control = None

    @staticmethod
    def check(ok):
        if not ok:
            raise C.WinError(C.get_last_error())

    def transfer(self, data, write):
        done, end = 0, time.monotonic() + 5
        while done < C.sizeof(data):
            count = W.DWORD()
            ok = (self.api.WriteFile if write else self.api.ReadFile)(
                self.control, C.byref(data, done), C.sizeof(data) - done, C.byref(count), None)
            done += count.value
            if not ok and C.get_last_error() not in (232, 536):
                self.check(False)
            assert time.monotonic() < end, 'private control deadline'
            if not count.value:
                time.sleep(.001)

    def publish(self, process, role, wrong_birth=False):
        current = self.api.GetCurrentProcess()
        remote = W.HANDLE()
        self.check(self.api.DuplicateHandle(current, process, self.broker._handle, C.byref(remote), 0x101040, False, 0))
        born, exited, kernel, user = (C.c_uint64() for _ in range(4))
        self.check(self.api.GetProcessTimes(process, C.byref(born), C.byref(exited), C.byref(kernel), C.byref(user)))
        request = Request(magic=0x474c4350, version=2, op=1, uid=2000 if role == '@launcher' else 1000,
                          process=remote.value, birth=born.value + int(wrong_birth), package=role.encode())
        self.transfer(request, True)
        status = C.c_int32(-1)
        self.transfer(status, False)
        return status.value == 0

    def spawn(self, args, output, environment, context, inherited=(), role=None):
        import msvcrt
        nul = open(os.devnull, 'rb')
        handles = [msvcrt.get_osfhandle(nul.fileno()), msvcrt.get_osfhandle(output.fileno()), *inherited]
        size = C.c_size_t()
        self.api.InitializeProcThreadAttributeList(None, 1, 0, C.byref(size))
        storage = C.create_string_buffer(size.value)
        self.check(self.api.InitializeProcThreadAttributeList(storage, 1, 0, C.byref(size)))
        array = (W.HANDLE * len(handles))(*handles)
        info = ProcessInfo()
        try:
            for handle in handles:
                self.check(self.api.SetHandleInformation(handle, 1, 1))
            self.check(self.api.UpdateProcThreadAttribute(storage, 0, 0x20002, array, C.sizeof(array), None, None))
            startup = Startup(cb=C.sizeof(Startup), flags=0x100, size=C.sizeof(context),
                              data=C.addressof(context), input=handles[0], output=handles[1], error=handles[1],
                              attributes=C.addressof(storage))
            block = C.create_unicode_buffer('\0'.join(k + '=' + v for k, v in sorted(environment.items())) + '\0\0')
            command = C.create_unicode_buffer(subprocess.list2cmdline([str(a) for a in args]))
            self.check(self.api.CreateProcessW(None, command, None, None, True, 0x80404, block, None, C.byref(startup), C.byref(info)))
            child = Child(self, info, output)
            self.children.append(child)
            self.job.own(child)  # exact retained suspended process, before any instruction
            if role:
                assert self.publish(child._handle, role), 'private fixture admission'
            assert self.api.ResumeThread(info.thread) == 1, 'exact suspended count before resume after ACK'
            return child
        except BaseException:
            if info.process:
                if child.poll() is None:
                    child.kill()
                child.wait(5)  # retained owner survives any uncertain rollback
            raise
        finally:
            if info.thread:
                self.check(self.api.CloseHandle(info.thread))
            for handle in handles:
                self.check(self.api.SetHandleInformation(handle, 1, 0))
            self.api.DeleteProcThreadAttributeList(storage)
            nul.close()

    def begin(self, broker, endpoint, output, environment):
        # Match production Channel: unpredictable private name, owner-only DACL,
        # first-instance, byte-mode/nonblocking, remote clients rejected.
        advapi = C.WinDLL('advapi32', use_last_error=True)
        convert = advapi.ConvertStringSecurityDescriptorToSecurityDescriptorW
        convert.argtypes = [W.LPCWSTR, W.DWORD, C.c_void_p, C.c_void_p]
        descriptor = C.c_void_p()
        self.check(convert('D:P(A;;GA;;;OW)S:(ML;;NW;;;ME)', 1, C.byref(descriptor), None))
        class Security(C.Structure):
            _fields_ = [('length', W.DWORD), ('descriptor', C.c_void_p), ('inherit', W.BOOL)]
        security = Security(C.sizeof(Security), descriptor, False)
        name = '\\\\.\\pipe\\gesher-private-launch-' + uuid.uuid4().hex + uuid.uuid4().hex
        server = self.api.CreateNamedPipeW(name, 0x80003, 9, 1, 4096, 4096, 0, C.byref(security))
        local_free = self.api.LocalFree
        local_free.argtypes, local_free.restype = [C.c_void_p], C.c_void_p
        local_free(descriptor)
        self.check(server != C.c_void_p(-1).value)
        try:
            self.control = self.api.CreateFileW(name, 0xc0000000, 0, None, 3, 0, None)
            self.check(self.control != C.c_void_p(-1).value)
            self.check(self.api.ConnectNamedPipe(server, None) or C.get_last_error() == 535)
            mode = W.DWORD(1)
            self.check(self.api.SetNamedPipeHandleState(self.control, C.byref(mode), None, None))
            context = Context(magic=0x474c4358, version=3, kind=1, control=server)
            self.broker = self.spawn([broker, endpoint], output, environment, context, [server])
            current = self.api.GetCurrentProcess()
            assert not self.publish(current, '@launcher', True), 'wrong creation-time rejected'
            assert self.publish(current, '@launcher'), 'actual controller admitted'
            return self.broker
        finally:
            self.check(self.api.CloseHandle(server))

    def close(self):
        for child in self.children:
            if child.poll() is None:
                child.kill()
            child.wait(5)
            self.check(self.api.CloseHandle(child._handle))
        if self.control:
            self.check(self.api.CloseHandle(self.control))
