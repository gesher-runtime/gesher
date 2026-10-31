"""Pure SOURCE model, not a process runner, GC root or native lifetime proof.

No command text or process/file/clock APIs. A future runner must supply actual
receipts and independently implement native quarantine and a real Normal root.
"""
from dataclasses import dataclass
from enum import Enum

SESSION_MS = 330_000
SETUP_MS = 90_000
HOLD_MS = 180_000  # NEW diagnostic only; old five 15s modes are untouched.
CLEANUP_RESERVE_MS = 30_000
PIPE_BYTES_PER_ROLE = 1_048_576
READY_BYTES = 512
QUALIFICATION_KEYS = ("crt_owner", "wct", "private_symbols", "full")
GALLERY = ("NOT_DISABLED", "NOT_EXHAUSTIVE")


class ProtocolError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise ProtocolError(message)


class Phase(str, Enum):
    PREPARED = "PREPARED"
    PREWARM_COMPLETE = "PREWARM_COMPLETE"
    HOLD_READY = "HOLD_READY"
    CAPTURE_COMPLETE = "CAPTURE_COMPLETE"
    RELEASED = "RELEASED"
    COMPLETE = "COMPLETE"
    FAILED = "FAILED"


class ProcessState(str, Enum):
    NOT_STARTED = "NOT_STARTED"
    OMITTED = "OMITTED"
    RUNNING = "RUNNING"
    EXITED = "EXITED"
    DISPOSED = "DISPOSED"


@dataclass(frozen=True)
class Role:
    state: ProcessState = ProcessState.NOT_STARTED
    pid: int | None = None
    exit_code: int | None = None
    pipes_closed: bool = False


def dword(value):
    require(type(value) is int and 0 < value <= 0xFFFFFFFF, "nonzero DWORD identity")
    return value


@dataclass(frozen=True)
class NativeLifetime:
    """Native failure ownership, distinct from the controller's CLR root.

    True completion flags require actual retained kernel handles. A terminated
    child permits OS reclamation, never a claim that native cleanup executed.
    """
    handler_complete: bool = False
    writer_complete: bool = False
    child_terminated: bool = False

    def disposition(self):
        require(all(type(value) is bool for value in (
            self.handler_complete, self.writer_complete, self.child_terminated)), "native receipt types")
        if self.child_terminated:
            return "OS_RECLAIM_AFTER_EXACT_CHILD_TERMINATION"
        if self.handler_complete and self.writer_complete:
            return "NATIVE_STORAGE_MAY_RETIRE_AND_HANDLER_MAY_RESTORE"
        return "QUARANTINE_STORAGE_EVENTS_AND_HANDLER_IN_LIVE_CHILD"


class Session:
    """One monotonic timeline and immutable per-role PID identities.

    Root reservation here is metadata only. Real implementation must allocate
    Normal GCHandle before any resource and quarantine native storage until
    both native threads truly end, or until exact owned child termination.
    """
    def __init__(self, *, optional_help=False, prewarm=True):
        require(type(optional_help) is bool and type(prewarm) is bool, "role configuration")
        self.phase = Phase.PREPARED
        self.roles = {name: Role() for name in ("child", "imports", "help", "prewarm", "capture")}
        for name, enabled in (("help", optional_help), ("prewarm", prewarm)):
            if not enabled:
                self.roles[name] = Role(ProcessState.OMITTED)
        self.last_ms = 0
        self.hold_deadline = None
        self.root_reserved = True
        self.handles = 0
        self.closed_handles = 0
        self.hold_ids = None
        self.failure = None

    def time(self, now):
        # Cleanup receipts remain recordable AFTER a deadline; that cannot make
        # the diagnostic succeed or permit resource closure before completion.
        require(type(now) is int and self.last_ms <= now <= (1 << 63) - 1, "monotonic event time")
        self.last_ms = now

    def active(self):
        require(self.root_reserved and self.phase not in (Phase.FAILED, Phase.COMPLETE),
                "session is terminal or root released")

    def copied_handle(self, now):
        self.active()
        self.time(now)
        require(self.phase == Phase.PREPARED and now <= SETUP_MS, "copy outside preparation")
        require(self.handles < 128, "copied handle cap")
        self.handles += 1

    def start(self, name, pid, now):
        self.active()
        self.time(now)
        require(name in self.roles and self.roles[name].state == ProcessState.NOT_STARTED,
                "role cannot be started/replaced")
        dword(pid)
        require(all(role.pid != pid for role in self.roles.values()), "duplicate process identity")
        if name == "capture":
            require(self.phase == Phase.HOLD_READY and now < self.hold_deadline, "capture phase/budget")
            require(self.roles["child"].state == ProcessState.RUNNING, "capture exact live child")
        else:
            require(self.phase == Phase.PREPARED and now <= SETUP_MS, "startup phase/budget")
            if name == "prewarm":
                require(self.roles["child"].state == ProcessState.RUNNING, "prewarm exact child")
                require(self.done("help"), "help must finish before target prewarm")
        self.roles[name] = Role(ProcessState.RUNNING, pid)

    def exited(self, name, pid, exit_code, *, kernel_complete, now):
        self.time(now)
        require(name in self.roles, "unknown role")
        role = self.roles[name]
        require(role.state == ProcessState.RUNNING and role.pid == dword(pid), "exit identity/state")
        require(kernel_complete is True and type(exit_code) is int, "true kernel completion required")
        self.roles[name] = Role(ProcessState.EXITED, pid, exit_code)

    def dispose(self, name, *, pipes_closed, now):
        self.time(now)
        require(name in self.roles, "unknown role")
        role = self.roles[name]
        require(role.state == ProcessState.EXITED and pipes_closed is True, "dispose before completion/drain")
        self.roles[name] = Role(ProcessState.DISPOSED, role.pid, role.exit_code, True)

    def done(self, name):
        role = self.roles[name]
        return role.state == ProcessState.OMITTED or (
            role.state == ProcessState.DISPOSED and role.exit_code == 0 and role.pipes_closed)

    def prepared(self, *, target_ready, stage_rechecked, symbols_prepared, now):
        self.active()
        self.time(now)
        require(self.phase == Phase.PREPARED and now <= SETUP_MS, "preparation phase/budget")
        require(target_ready is True and stage_rechecked is True and symbols_prepared is True,
                "actual preparation receipts required")
        require(self.roles["child"].state == ProcessState.RUNNING and self.done("imports") and self.done("help") and self.done("prewarm"),
                "preparation role completion")
        self.phase = Phase.PREWARM_COMPLETE

    def held(self, pid, writer_tid, handler_tid, *, both_native_pending, now):
        self.active()
        self.time(now)
        require(self.phase == Phase.PREWARM_COMPLETE, "hold phase")
        require(self.roles["child"].state == ProcessState.RUNNING, "hold exact live child")
        ids = tuple(dword(value) for value in (pid, writer_tid, handler_tid))
        require(pid == self.roles["child"].pid and writer_tid != handler_tid, "owned/distinct hold identities")
        require(both_native_pending is True, "native handles must be genuinely pending")
        deadline = min(now + HOLD_MS, SESSION_MS - CLEANUP_RESERVE_MS)
        require(now < deadline, "no diagnostic hold budget")
        self.hold_ids = ids
        self.hold_deadline = deadline
        self.phase = Phase.HOLD_READY

    def captured(self, *, raw_evidence_recorded, now):
        self.active()
        self.time(now)
        require(self.phase == Phase.HOLD_READY and now < self.hold_deadline, "capture deadline/phase")
        require(self.roles["child"].state == ProcessState.RUNNING, "capture must leave same child live")
        require(self.done("capture") and raw_evidence_recorded is True, "capture completion/raw record")
        self.phase = Phase.CAPTURE_COMPLETE

    def released(self, *, release_delivered, now):
        self.active()
        self.time(now)
        require(self.phase == Phase.CAPTURE_COMPLETE and now < self.hold_deadline, "release deadline/phase")
        require(self.roles["child"].state == ProcessState.RUNNING, "release exact live child")
        require(release_delivered is True, "release must be delivered to same child")
        self.phase = Phase.RELEASED

    def close_copied_handles(self, count, now):
        self.time(now)
        require(self.phase in (Phase.RELEASED, Phase.COMPLETE, Phase.FAILED), "handle close phase")
        require(type(count) is int and 0 <= count <= self.handles - self.closed_handles, "closed handle count")
        require(self.all_processes_closed(), "file handles cannot close while a process may live")
        self.closed_handles += count

    def all_processes_closed(self):
        return all(role.state in (ProcessState.NOT_STARTED, ProcessState.OMITTED, ProcessState.DISPOSED)
                   for role in self.roles.values())

    def complete(self, now):
        self.active()
        self.time(now)
        require(now <= SESSION_MS, "session success deadline")
        require(self.phase == Phase.RELEASED and all(self.done(name) for name in self.roles),
                "all role results/completion required")
        require(self.handles == self.closed_handles, "copied handles still open")
        self.phase = Phase.COMPLETE

    def fail(self, reason, now):
        self.time(now)
        require(self.phase not in (Phase.COMPLETE, Phase.FAILED) and type(reason) is str and 0 < len(reason) <= 128,
                "failure record")
        self.phase = Phase.FAILED
        self.failure = reason

    def release_root(self):
        # Idempotent model of one real Free, not an actual GCHandle operation.
        if not self.root_reserved:
            return False
        require(self.phase in (Phase.COMPLETE, Phase.FAILED), "root release before terminal phase")
        require(self.all_processes_closed() and self.handles == self.closed_handles,
                "root must remain retained through live resources")
        self.root_reserved = False
        return True

    def evidence(self):
        return {"phase": self.phase.value, "root_retained": self.root_reserved,
                "roles": {name: role.state.value for name, role in self.roles.items()},
                "gallery": GALLERY, "qualification": dict.fromkeys(QUALIFICATION_KEYS, False),
                "hold_is_owner_proof": False, "metadata_only": True}
