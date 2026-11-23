"""Owned Windows process-tree measurement; no application-specific behavior.

Requires 64-bit Python and Windows 10+. Creation-time JOB_LIST assignment avoids
the unowned-child interval of CreateProcess followed by AssignProcessToJobObject.
The job is never inheritable, permits no breakaway, and kills members on close.
See docs/performance-benchmarks.md for measurement boundaries and API sources.
"""
from __future__ import annotations

import ctypes as c
import os
from pathlib import Path
import shutil
import subprocess
import time

from contract import aggregate_snapshot

U32, U64, I64, SIZE, HANDLE = c.c_uint32, c.c_uint64, c.c_int64, c.c_size_t, c.c_void_p
BOOL, PTR = c.c_int32, c.c_void_p


class IO(c.Structure):
    _fields_ = [(name, U64) for name in ("read_operations", "write_operations", "other_operations",
                                        "read_bytes", "write_bytes", "other_bytes")]


class Accounting(c.Structure):
    _fields_ = [(name, I64) for name in ("user", "kernel", "period_user", "period_kernel")] + [
        (name, U32) for name in ("faults", "total_processes", "active_processes", "terminated_processes")]


class AccountingIO(c.Structure):
    _fields_ = [("basic", Accounting), ("io", IO)]


class BasicLimits(c.Structure):
    _fields_ = [("process_time", I64), ("job_time", I64), ("flags", U32),
                ("min_ws", SIZE), ("max_ws", SIZE), ("active", U32),
                ("affinity", SIZE), ("priority", U32), ("scheduling", U32)]


class Limits(c.Structure):
    _fields_ = [("basic", BasicLimits), ("io", IO), ("process_memory", SIZE), ("job_memory", SIZE),
                ("peak_process_memory", SIZE), ("peak_job_memory", SIZE)]


class Startup(c.Structure):
    _fields_ = [("cb", U32), ("reserved", c.c_wchar_p), ("desktop", c.c_wchar_p), ("title", c.c_wchar_p)] + [
        (name, U32) for name in ("x", "y", "xsize", "ysize", "xchars", "ychars", "fill", "flags")] + [
        ("show", c.c_uint16), ("reserved_count", c.c_uint16), ("reserved_bytes", PTR),
        ("stdin", HANDLE), ("stdout", HANDLE), ("stderr", HANDLE)]


class StartupEx(c.Structure):
    _fields_ = [("startup", Startup), ("attributes", PTR)]


class ProcessInfo(c.Structure):
    _fields_ = [("process", HANDLE), ("thread", HANDLE), ("pid", U32), ("tid", U32)]


class Memory(c.Structure):
    _fields_ = [("cb", U32), ("faults", U32)] + [(name, SIZE) for name in (
        "peak_ws", "ws", "peak_paged", "paged", "peak_nonpaged", "nonpaged", "pagefile", "peak_pagefile", "private")]


class ProcessEntry(c.Structure):
    _fields_ = [("size", U32), ("usage", U32), ("pid", U32), ("heap", SIZE),
                ("module", U32), ("threads", U32), ("parent", U32), ("priority", c.c_int32),
                ("flags", U32), ("exe", c.c_wchar * 260)]


class SystemInfo(c.Structure):
    _fields_ = [("architecture_reserved", U32), ("page_size", U32),
                ("minimum_address", PTR), ("maximum_address", PTR), ("active_mask", SIZE),
                ("processor_count", U32), ("processor_type", U32), ("allocation_granularity", U32),
                ("processor_level", c.c_uint16), ("processor_revision", c.c_uint16)]


class WindowsJob:
    def __init__(self, command: list[str], cwd: Path, env: dict[str, str], output: Path,
                 *, residency: bool = False, exclude_root: bool = False):
        if os.name != "nt" or c.sizeof(PTR) != 8:
            raise RuntimeError("Windows job collection requires 64-bit Python on Windows")
        import msvcrt
        self.kernel = k = c.WinDLL("kernel32", use_last_error=True)
        self.psapi = p = c.WinDLL("psapi", use_last_error=True)
        signatures = {
            "CreateJobObjectW": ([PTR, c.c_wchar_p], HANDLE),
            "CloseHandle": ([HANDLE], BOOL),
            "SetInformationJobObject": ([HANDLE, c.c_int, PTR, U32], BOOL),
            "QueryInformationJobObject": ([HANDLE, c.c_int, PTR, U32, c.POINTER(U32)], BOOL),
            "TerminateJobObject": ([HANDLE, U32], BOOL),
            "InitializeProcThreadAttributeList": ([PTR, U32, U32, c.POINTER(SIZE)], BOOL),
            "UpdateProcThreadAttribute": ([PTR, U32, SIZE, PTR, SIZE, PTR, PTR], BOOL),
            "DeleteProcThreadAttributeList": ([PTR], None),
            "CreateProcessW": ([c.c_wchar_p, c.c_wchar_p, PTR, PTR, BOOL, U32, PTR, c.c_wchar_p,
                                c.POINTER(StartupEx), c.POINTER(ProcessInfo)], BOOL),
            "ResumeThread": ([HANDLE], U32),
            "WaitForSingleObject": ([HANDLE, U32], U32),
            "GetExitCodeProcess": ([HANDLE, c.POINTER(U32)], BOOL),
            "OpenProcess": ([U32, BOOL, U32], HANDLE),
            "IsProcessInJob": ([HANDLE, HANDLE, c.POINTER(BOOL)], BOOL),
            "GetProcessTimes": ([HANDLE, PTR, PTR, PTR, PTR], BOOL),
            "GetProcessHandleCount": ([HANDLE, c.POINTER(U32)], BOOL),
            "GetProcessIoCounters": ([HANDLE, c.POINTER(IO)], BOOL),
            "GetSystemInfo": ([c.POINTER(SystemInfo)], None),
            "CreateToolhelp32Snapshot": ([U32, U32], HANDLE),
            "Process32FirstW": ([HANDLE, c.POINTER(ProcessEntry)], BOOL),
            "Process32NextW": ([HANDLE, c.POINTER(ProcessEntry)], BOOL),
        }
        for name, (arguments, result) in signatures.items():
            getattr(k, name).argtypes, getattr(k, name).restype = arguments, result
        p.GetProcessMemoryInfo.argtypes, p.GetProcessMemoryInfo.restype = [HANDLE, c.POINTER(Memory), U32], BOOL
        p.QueryWorkingSet.argtypes, p.QueryWorkingSet.restype = [HANDLE, PTR, U32], BOOL
        system = SystemInfo()
        k.GetSystemInfo(c.byref(system))
        if not system.page_size:
            raise RuntimeError("Windows did not report a page size")
        self.page_size = system.page_size
        self.job = self.process = None
        self.residency = residency
        self.exclude_root = exclude_root
        self.observed = {}
        # Authenticated while alive, independent of optional sampling. Holding
        # these handles retains the exact process object through normal exit.
        self.registered_handles = {}
        self.stdout = self.stderr = self.stdin = None
        info = ProcessInfo()
        attributes = None
        initialized = False
        try:
            self.job = k.CreateJobObjectW(None, None)
            self.check(self.job, "CreateJobObjectW")
            limits = Limits()
            limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE; no BREAKAWAY.
            self.check(k.SetInformationJobObject(self.job, 9, c.byref(limits), c.sizeof(limits)), "SetInformationJobObject")
            self.stdout = (output / "stdout.log").open("wb")
            self.stderr = (output / "stderr.log").open("wb")
            self.stdin = open(os.devnull, "rb")
            handles = (HANDLE * 3)(*[msvcrt.get_osfhandle(stream.fileno()) for stream in
                                    (self.stdin, self.stdout, self.stderr)])
            for handle in handles:
                os.set_handle_inheritable(handle, True)
            size = SIZE()
            k.InitializeProcThreadAttributeList(None, 2, 0, c.byref(size))
            if not size.value:
                self.fail("InitializeProcThreadAttributeList size")
            attributes = c.create_string_buffer(size.value)
            self.check(k.InitializeProcThreadAttributeList(attributes, 2, 0, c.byref(size)), "InitializeProcThreadAttributeList")
            initialized = True
            jobs = (HANDLE * 1)(self.job)
            self.check(k.UpdateProcThreadAttribute(attributes, 0, 0x20002, handles, c.sizeof(handles), None, None), "HANDLE_LIST")
            self.check(k.UpdateProcThreadAttribute(attributes, 0, 0x2000D, jobs, c.sizeof(jobs), None, None), "JOB_LIST")
            start = StartupEx()
            start.startup.cb = c.sizeof(start)
            start.startup.flags = 0x100  # STARTF_USESTDHANDLES
            start.startup.stdin, start.startup.stdout, start.startup.stderr = handles
            start.attributes = c.cast(attributes, PTR)
            executable = shutil.which(command[0], path=env.get("PATH"))
            if not executable:
                raise FileNotFoundError(command[0])
            if any("\0" in value or "=" in key or not key for key, value in env.items()):
                raise ValueError("Invalid environment block")
            environment = c.create_unicode_buffer("\0".join(f"{key}={value}" for key, value in
                                                             sorted(env.items(), key=lambda row: row[0].upper())) + "\0")
            command_line = c.create_unicode_buffer(subprocess.list2cmdline(command))
            self.check(k.CreateProcessW(executable, command_line, None, None, True,
                                        0x80000 | 0x400 | 0x4, environment, str(cwd),
                                        c.byref(start), c.byref(info)), "CreateProcessW with owned job")
            self.process, self.pid = info.process, info.pid
            belongs = BOOL()
            self.check(k.IsProcessInJob(self.process, self.job, c.byref(belongs)), "IsProcessInJob")
            if not belongs.value:
                raise RuntimeError("New workload is outside its measurement job")
            if k.ResumeThread(info.thread) == 0xffffffff:
                self.fail("ResumeThread")
        except BaseException:
            self.close()
            raise
        finally:
            if info.thread:
                k.CloseHandle(info.thread)
            if initialized:
                k.DeleteProcThreadAttributeList(attributes)
            for stream in (self.stdin, self.stdout, self.stderr):
                if stream is not None and not stream.closed:
                    os.set_handle_inheritable(msvcrt.get_osfhandle(stream.fileno()), False)

    @staticmethod
    def fail(operation):
        error = c.get_last_error()
        raise OSError(error, f"{operation}: {c.FormatError(error).strip()}")

    def check(self, result, operation):
        if not result:
            self.fail(operation)

    def poll(self):
        waited = self.kernel.WaitForSingleObject(self.process, 0)
        if waited == 258:
            return None
        if waited != 0:
            self.fail("WaitForSingleObject")
        code = U32()
        self.check(self.kernel.GetExitCodeProcess(self.process, c.byref(code)), "GetExitCodeProcess")
        return code.value

    def query(self, kind, structure):
        value = structure()
        self.check(self.kernel.QueryInformationJobObject(self.job, kind, c.byref(value), c.sizeof(value), None),
                   "QueryInformationJobObject")
        return value

    def pids(self):
        count = 64
        for _ in range(12):
            data = c.create_string_buffer(8 + count * c.sizeof(SIZE))
            if self.kernel.QueryInformationJobObject(self.job, 3, data, len(data), None):
                assigned, listed = (U32 * 2).from_buffer(data)
                if listed > count or assigned > listed:
                    count = max(count * 2, assigned)
                    continue
                return list((SIZE * listed).from_buffer(data, 8))
            if c.get_last_error() != 234:  # ERROR_MORE_DATA
                self.fail("JobObjectBasicProcessIdList")
            count *= 2
        raise RuntimeError("Process list kept changing beyond the bounded capture budget")

    def thread_counts(self):
        snapshot = self.kernel.CreateToolhelp32Snapshot(2, 0)
        if snapshot == c.c_void_p(-1).value:
            self.fail("CreateToolhelp32Snapshot")
        try:
            entry, counts = ProcessEntry(), {}
            entry.size = c.sizeof(entry)
            found = self.kernel.Process32FirstW(snapshot, c.byref(entry))
            while found:
                counts[entry.pid] = entry.threads
                found = self.kernel.Process32NextW(snapshot, c.byref(entry))
            if c.get_last_error() != 18:  # ERROR_NO_MORE_FILES
                self.fail("Process32NextW")
            return counts
        finally:
            self.kernel.CloseHandle(snapshot)

    def private_residency(self, handle):
        count = 32768
        for _ in range(9):
            data = (SIZE * (count + 1))()
            if self.psapi.QueryWorkingSet(handle, data, c.sizeof(data)):
                if data[0] > count:
                    count = int(data[0]) + 1024
                    continue
                # PSAPI_WORKING_SET_BLOCK: Shared is bit 8. No page contents read.
                return sum(not bool(flags & 0x100) for flags in data[1:1 + data[0]]) * self.page_size
            if c.get_last_error() not in (24, 122):
                self.fail("QueryWorkingSet")
            count = max(count * 2, int(data[0]) + 1024)
        raise RuntimeError("Working set changed beyond the bounded capture budget")

    def sample(self):
        started = time.perf_counter()
        observer_before = self.observer_counters() if self.exclude_root else None
        accounting = self.query(8, AccountingIO)
        observer_after = self.observer_counters() if self.exclude_root else None
        limits = self.query(9, Limits)
        pids, threads, processes, errors = self.pids(), self.thread_counts(), [], []
        for pid in pids:
            handle = self.kernel.OpenProcess(0x100000 | 0x1000 | 0x400 | 0x10, False, pid)
            if not handle:
                errors.append({"pid": pid, "operation": "OpenProcess", "winerror": c.get_last_error()})
                continue
            try:
                belongs = BOOL()
                self.check(self.kernel.IsProcessInJob(handle, self.job, c.byref(belongs)), "IsProcessInJob sample")
                if not belongs.value:
                    errors.append({"pid": pid, "operation": "PID no longer belongs to job"})
                    continue
                created, exited, kernel, user = U64(), U64(), U64(), U64()
                self.check(self.kernel.GetProcessTimes(handle, c.byref(created), c.byref(exited),
                                                      c.byref(kernel), c.byref(user)), "GetProcessTimes")
                if self.kernel.WaitForSingleObject(handle, 0) == 0:
                    continue
                memory, handles = Memory(), U32()
                memory.cb = c.sizeof(memory)
                self.check(self.psapi.GetProcessMemoryInfo(handle, c.byref(memory), c.sizeof(memory)), "GetProcessMemoryInfo")
                self.check(self.kernel.GetProcessHandleCount(handle, c.byref(handles)), "GetProcessHandleCount")
                item = {"pid": pid, "created_100ns": created.value, "private_commit_bytes": memory.private,
                        "working_set_bytes": memory.ws, "private_resident_bytes": None,
                        "handles": handles.value, "threads": threads.get(pid),
                        "measurement_observer": self.exclude_root and pid == self.pid}
                if self.residency:
                    item["private_resident_bytes"] = self.private_residency(handle)
                processes.append(item)
                self.observed[(pid, created.value)] = item
            except OSError as error:
                errors.append({"pid": pid, "operation": str(error)})
            finally:
                self.kernel.CloseHandle(handle)
        after = self.pids()
        complete = not errors and set(pids) == set(after) == {item["pid"] for item in processes}
        return {"at_monotonic": started, "collection_ms": (time.perf_counter() - started) * 1000,
                "job": {"user_cpu_ms": accounting.basic.user / 10000,
                        "kernel_cpu_ms": accounting.basic.kernel / 10000,
                        "page_faults": accounting.basic.faults, "total_processes": accounting.basic.total_processes,
                        "active_processes": accounting.basic.active_processes,
                        "lifetime_peak_commit_bytes": limits.peak_job_memory,
                        **{name: getattr(accounting.io, name) for name, _ in IO._fields_}},
                "observer_before": observer_before, "observer_after": observer_after,
                "exclude_root_observer": self.exclude_root,
                "processes": processes, "totals": aggregate_snapshot([item for item in processes if not item["measurement_observer"]]),
                "live_snapshot_complete": complete, "errors": errors}

    def observer_counters(self):
        created, exited, kernel, user, io, memory = U64(), U64(), U64(), U64(), IO(), Memory()
        memory.cb = c.sizeof(memory)
        self.check(self.kernel.GetProcessTimes(self.process, c.byref(created), c.byref(exited),
                                              c.byref(kernel), c.byref(user)), "GetProcessTimes observer")
        self.check(self.kernel.GetProcessIoCounters(self.process, c.byref(io)), "GetProcessIoCounters observer")
        self.check(self.psapi.GetProcessMemoryInfo(self.process, c.byref(memory), c.sizeof(memory)), "GetProcessMemoryInfo observer")
        return {"user_cpu_ms": user.value / 10000, "kernel_cpu_ms": kernel.value / 10000,
                "page_faults": memory.faults, **{name: getattr(io, name) for name, _ in IO._fields_}}

    def verify_participants(self, participants, *, allow_observed_exit=False):
        """Validate actual handles, not process names or a reused numeric PID."""
        if not isinstance(participants, list):
            raise ValueError("Invalid workload participant list")
        identities = set()
        for item in participants:
            if (not isinstance(item, dict) or type(item.get("pid")) is not int or item["pid"] <= 0 or
                    type(item.get("created_100ns")) is not int or item["created_100ns"] <= 0):
                raise ValueError("Invalid workload process identity")
            identity = (item["pid"], item["created_100ns"])
            if identity in identities:
                raise ValueError("Duplicate workload process identity")
            identities.add(identity)
            handle = self.kernel.OpenProcess(0x100000 | 0x1000, False, item["pid"])
            if not handle:
                if allow_observed_exit and identity in self.observed:
                    continue
                self.fail("OpenProcess required workload participant")
            try:
                created, exited, kernel, user = U64(), U64(), U64(), U64()
                self.check(self.kernel.GetProcessTimes(handle, c.byref(created), c.byref(exited),
                                                      c.byref(kernel), c.byref(user)), "GetProcessTimes participant")
                if created.value != item["created_100ns"]:
                    if allow_observed_exit and identity in self.observed:
                        continue  # This PID now names a different process; never touch it.
                    raise ValueError("Workload participant PID was reused")
                if (allow_observed_exit and identity in self.observed and
                        self.kernel.WaitForSingleObject(handle, 0) == 0):
                    continue  # This exact process was authenticated before its normal exit.
                belongs = BOOL()
                self.check(self.kernel.IsProcessInJob(handle, self.job, c.byref(belongs)), "IsProcessInJob participant")
                if not belongs.value:
                    raise ValueError("Workload attached to a process outside its owned measurement job")
                if not allow_observed_exit:
                    if self.kernel.WaitForSingleObject(handle, 0) != 258:
                        raise ValueError("Required workload participant exited before registration")
                    if identity not in self.registered_handles:
                        self.registered_handles[identity] = handle
                        handle = None
                self.observed[identity] = item
            finally:
                if handle:
                    self.kernel.CloseHandle(handle)

    def close(self):
        """Terminate only owned members; close every handle even if cleanup fails."""
        error = None
        witnesses = []
        try:
            if self.job:
                # Hold real handles across termination. Reused numeric PIDs are
                # never cleanup authority, and descendants are waited as well.
                try:
                    for pid in self.pids():
                        handle = self.kernel.OpenProcess(0x100000 | 0x1000, False, pid)
                        if handle:
                            belongs = BOOL()
                            if self.kernel.IsProcessInJob(handle, self.job, c.byref(belongs)) and belongs.value:
                                witnesses.append(handle)
                            else:
                                self.kernel.CloseHandle(handle)
                except (OSError, RuntimeError) as caught:
                    error = caught
                if not self.kernel.TerminateJobObject(self.job, 125):
                    error = OSError(c.get_last_error(), "TerminateJobObject failed")
                deadline = time.monotonic() + 10
                for handle in witnesses + ([self.process] if self.process else []):
                    waited = self.kernel.WaitForSingleObject(handle, max(0, int((deadline - time.monotonic()) * 1000)))
                    if waited != 0:
                        error = RuntimeError("Owned workload member did not terminate within 10 seconds")
        finally:
            for handle in witnesses:
                self.kernel.CloseHandle(handle)
            for handle in self.registered_handles.values():
                self.kernel.CloseHandle(handle)
            self.registered_handles.clear()
            if self.process:
                self.kernel.CloseHandle(self.process)
                self.process = None
            if self.job:
                self.kernel.CloseHandle(self.job)
                self.job = None
            for stream in (self.stdin, self.stdout, self.stderr):
                if stream is not None:
                    stream.close()
        if error:
            raise error
