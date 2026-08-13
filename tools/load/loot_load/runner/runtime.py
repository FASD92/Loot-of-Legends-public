"""Process ownership and lifecycle for traffic workers.

The parent owns lifecycle and IPC only.  Gameplay sockets belong to worker
entry functions supplied by the workload layer.
"""

from __future__ import annotations

import multiprocessing
import queue
import time
from dataclasses import dataclass
from enum import Enum
from typing import Any, Callable


class RunnerPhase(str, Enum):
    CREATED = "Created"
    STARTING = "Starting"
    READY = "Ready"
    WARMUP = "Warmup"
    MEASURING = "Measuring"
    DRAINING = "Draining"
    MEASUREMENT_COMPLETE = "MeasurementComplete"
    ABORTED = "Aborted"


class IpcQueueSaturated(RuntimeError):
    pass


class WorkerCrashed(RuntimeError):
    def __init__(self, worker_id: int, detail: str = "") -> None:
        suffix = f": {detail}" if detail else ""
        super().__init__(f"worker {worker_id} exited unexpectedly{suffix}")
        self.worker_id = worker_id


class WorkerHeartbeatExpired(RuntimeError):
    def __init__(self, worker_id: int, gap_seconds: float) -> None:
        super().__init__(f"worker {worker_id} heartbeat gap is {gap_seconds:.3f}s")
        self.worker_id = worker_id
        self.gap_seconds = gap_seconds


@dataclass(frozen=True)
class WorkerAssignment:
    worker_id: int
    room_group_ids: tuple[int, ...]


def assign_room_groups(room_group_count: int, worker_count: int) -> tuple[WorkerAssignment, ...]:
    if room_group_count < 1:
        raise ValueError("room_group_count must be positive")
    if worker_count < 1 or worker_count > room_group_count:
        raise ValueError("worker_count must be between 1 and room_group_count")
    size, remainder = divmod(room_group_count, worker_count)
    assignments: list[WorkerAssignment] = []
    first = 0
    for worker_id in range(worker_count):
        count = size + (1 if worker_id < remainder else 0)
        room_groups = tuple(range(first, first + count))
        assignments.append(WorkerAssignment(worker_id, room_groups))
        first += count
    return tuple(assignments)


class WorkerEventSink:
    def __init__(self, event_queue: Any, worker_id: int) -> None:
        self._queue = event_queue
        self._worker_id = worker_id

    def emit(self, event_type: str, **fields: object) -> None:
        event = {
            "type": event_type,
            "workerId": self._worker_id,
            "monotonicNs": time.monotonic_ns(),
            **fields,
        }
        try:
            self._queue.put_nowait(event)
        except queue.Full as exc:
            raise IpcQueueSaturated(f"worker {self._worker_id} event queue is full") from exc


@dataclass
class WorkerContext:
    worker_id: int
    room_group_ids: tuple[int, ...]
    stop_event: Any
    warmup_event: Any
    measurement_event: Any
    draining_event: Any
    event_sink: WorkerEventSink
    heartbeat_interval_seconds: float
    _last_heartbeat: float = 0.0

    def heartbeat(self, *, force: bool = False) -> bool:
        now = time.monotonic()
        if not force and now - self._last_heartbeat < self.heartbeat_interval_seconds:
            return False
        self.event_sink.emit("HEARTBEAT", roomGroupIds=list(self.room_group_ids))
        self._last_heartbeat = now
        return True

    def wait_for_stop_with_heartbeat(self, timeout_seconds: float) -> bool:
        if timeout_seconds <= 0:
            raise ValueError("timeout_seconds must be positive")
        deadline = time.monotonic() + timeout_seconds
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return False
            if self.stop_event.wait(
                min(remaining, self.heartbeat_interval_seconds, 0.1)
            ):
                return True
            self.heartbeat()


WorkerEntry = Callable[[WorkerContext], None]


def _idle_worker(context: WorkerContext) -> None:
    while not context.stop_event.wait(min(context.heartbeat_interval_seconds, 0.1)):
        context.heartbeat()


def _worker_main(
    assignment: WorkerAssignment,
    stop_event: Any,
    warmup_event: Any,
    measurement_event: Any,
    draining_event: Any,
    event_queue: Any,
    heartbeat_interval_seconds: float,
    worker_entry: WorkerEntry,
) -> None:
    sink = WorkerEventSink(event_queue, assignment.worker_id)
    context = WorkerContext(
        worker_id=assignment.worker_id,
        room_group_ids=assignment.room_group_ids,
        stop_event=stop_event,
        warmup_event=warmup_event,
        measurement_event=measurement_event,
        draining_event=draining_event,
        event_sink=sink,
        heartbeat_interval_seconds=heartbeat_interval_seconds,
    )
    sink.emit("READY", roomGroupIds=list(assignment.room_group_ids))
    try:
        worker_entry(context)
    except Exception as exc:
        try:
            sink.emit("CRASH", exceptionType=type(exc).__name__, detail=str(exc)[:240])
        except IpcQueueSaturated:
            pass
        raise SystemExit(70) from exc
    try:
        sink.emit("STOPPED")
    except IpcQueueSaturated:
        pass


class ParentRuntime:
    def __init__(
        self,
        *,
        room_group_count: int,
        worker_count: int,
        worker_entry: WorkerEntry = _idle_worker,
        queue_capacity: int = 1024,
        heartbeat_interval_seconds: float = 1.0,
        heartbeat_timeout_seconds: float = 5.0,
        join_timeout_seconds: float = 2.0,
    ) -> None:
        if queue_capacity < worker_count:
            raise ValueError("queue_capacity must hold at least one event per worker")
        if heartbeat_interval_seconds <= 0:
            raise ValueError("heartbeat_interval_seconds must be positive")
        if heartbeat_timeout_seconds <= heartbeat_interval_seconds:
            raise ValueError("heartbeat_timeout_seconds must exceed heartbeat_interval_seconds")
        self._context = multiprocessing.get_context("spawn")
        self._assignments = assign_room_groups(room_group_count, worker_count)
        self._worker_entry = worker_entry
        self._event_queue = self._context.Queue(maxsize=queue_capacity)
        self._stop_event = self._context.Event()
        self._warmup_event = self._context.Event()
        self._measurement_event = self._context.Event()
        self._draining_event = self._context.Event()
        self._heartbeat_interval = heartbeat_interval_seconds
        self._heartbeat_timeout = heartbeat_timeout_seconds
        self._join_timeout = join_timeout_seconds
        self._phase = RunnerPhase.CREATED
        self._processes: list[multiprocessing.Process] = []
        self._events: list[dict[str, Any]] = []
        self._last_heartbeat: dict[int, float] = {}
        self._ready_workers: set[int] = set()
        self._closed = False

    @property
    def phase(self) -> RunnerPhase:
        return self._phase

    @property
    def events(self) -> tuple[dict[str, Any], ...]:
        return tuple(self._events)

    @property
    def processes(self) -> tuple[multiprocessing.Process, ...]:
        return tuple(self._processes)

    @property
    def assignments(self) -> tuple[WorkerAssignment, ...]:
        return self._assignments

    def start(self, *, timeout_seconds: float = 10.0) -> None:
        self._transition(RunnerPhase.CREATED, RunnerPhase.STARTING)
        for assignment in self._assignments:
            process = self._context.Process(
                target=_worker_main,
                args=(
                    assignment,
                    self._stop_event,
                    self._warmup_event,
                    self._measurement_event,
                    self._draining_event,
                    self._event_queue,
                    self._heartbeat_interval,
                    self._worker_entry,
                ),
                name=f"loot-load-worker-{assignment.worker_id}",
            )
            process.start()
            self._processes.append(process)
        deadline = time.monotonic() + timeout_seconds
        while len(self._ready_workers) != len(self._assignments):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("workers did not become ready")
            self.poll(timeout_seconds=min(remaining, 0.1))
        self._phase = RunnerPhase.READY

    def poll(self, *, timeout_seconds: float = 0.0) -> tuple[dict[str, Any], ...]:
        received: list[dict[str, Any]] = []
        if timeout_seconds > 0:
            try:
                received.append(self._event_queue.get(timeout=timeout_seconds))
            except queue.Empty:
                pass
        while True:
            try:
                received.append(self._event_queue.get_nowait())
            except queue.Empty:
                break
        now = time.monotonic()
        for event in received:
            self._events.append(event)
            worker_id = event.get("workerId")
            if not isinstance(worker_id, int):
                continue
            if event.get("type") == "READY":
                self._ready_workers.add(worker_id)
                self._last_heartbeat[worker_id] = now
            elif event.get("type") == "HEARTBEAT":
                self._last_heartbeat[worker_id] = now
            elif event.get("type") == "CRASH":
                raise WorkerCrashed(worker_id, str(event.get("detail", "")))
        self._check_processes()
        self._check_heartbeats(now)
        return tuple(received)

    def begin_warmup(self) -> None:
        self._transition(RunnerPhase.READY, RunnerPhase.WARMUP)
        self._warmup_event.set()

    def begin_measurement(self) -> None:
        self._transition(RunnerPhase.WARMUP, RunnerPhase.MEASURING)
        self._measurement_event.set()

    def begin_draining(self) -> None:
        self._transition(RunnerPhase.MEASURING, RunnerPhase.DRAINING)
        self._draining_event.set()

    def complete(self) -> None:
        if self._phase != RunnerPhase.DRAINING:
            raise RuntimeError(
                f"cannot transition {self._phase.value} -> {RunnerPhase.MEASUREMENT_COMPLETE.value}"
            )
        self._stop_event.set()
        stuck_workers = self._join_workers()
        if stuck_workers:
            self._phase = RunnerPhase.ABORTED
            raise WorkerCrashed(stuck_workers[0], "did not stop during draining")
        self._phase = RunnerPhase.MEASUREMENT_COMPLETE

    def abort(self) -> None:
        if self._phase == RunnerPhase.MEASUREMENT_COMPLETE:
            return
        self._phase = RunnerPhase.ABORTED
        self._stop_event.set()
        self._warmup_event.set()
        self._measurement_event.set()
        self._draining_event.set()
        self._join_workers()

    def close(self) -> None:
        if self._closed:
            return
        if self._phase not in {RunnerPhase.MEASUREMENT_COMPLETE, RunnerPhase.ABORTED}:
            self.abort()
        else:
            self._stop_event.set()
            self._join_workers()
        self._event_queue.close()
        self._event_queue.join_thread()
        self._closed = True

    def _transition(self, expected: RunnerPhase, target: RunnerPhase) -> None:
        if self._phase != expected:
            raise RuntimeError(f"cannot transition {self._phase.value} -> {target.value}")
        self._phase = target

    def _check_processes(self) -> None:
        if self._phase in {RunnerPhase.MEASUREMENT_COMPLETE, RunnerPhase.ABORTED}:
            return
        for assignment, process in zip(self._assignments, self._processes, strict=True):
            if process.exitcode is not None and not self._stop_event.is_set():
                raise WorkerCrashed(assignment.worker_id, f"exit code {process.exitcode}")

    def _check_heartbeats(self, now: float) -> None:
        if self._phase not in {
            RunnerPhase.READY,
            RunnerPhase.WARMUP,
            RunnerPhase.MEASURING,
            RunnerPhase.DRAINING,
        }:
            return
        for worker_id, last in self._last_heartbeat.items():
            gap = now - last
            if gap > self._heartbeat_timeout:
                raise WorkerHeartbeatExpired(worker_id, gap)

    def _join_workers(self) -> list[int]:
        stuck_workers: list[int] = []
        for assignment, process in zip(self._assignments, self._processes, strict=True):
            process.join(timeout=self._join_timeout)
            if process.is_alive():
                process.terminate()
                process.join(timeout=self._join_timeout)
                stuck_workers.append(assignment.worker_id)
        return stuck_workers
