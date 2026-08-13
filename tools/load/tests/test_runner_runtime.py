from __future__ import annotations

import multiprocessing
import time
import unittest


try:
    from tools.load.loot_load.runner.runtime import (
        IpcQueueSaturated,
        ParentRuntime,
        RunnerPhase,
        WorkerCrashed,
        WorkerEventSink,
        WorkerHeartbeatExpired,
        assign_room_groups,
    )
except ModuleNotFoundError:
    IpcQueueSaturated = None
    ParentRuntime = None
    RunnerPhase = None
    WorkerCrashed = None
    WorkerEventSink = None
    WorkerHeartbeatExpired = None
    assign_room_groups = None


def crashing_worker(_context):
    time.sleep(0.05)
    raise RuntimeError("expected crash")


def quiet_worker(context):
    while not context.stop_event.wait(0.01):
        context.heartbeat()


def silent_worker(context):
    context.stop_event.wait(1)


def uncooperative_worker(_context):
    time.sleep(1)


def phase_observing_worker(context):
    for event, name in (
        (context.warmup_event, "WARMUP_SEEN"),
        (context.measurement_event, "MEASUREMENT_SEEN"),
        (context.draining_event, "DRAINING_SEEN"),
    ):
        if not event.wait(1):
            raise TimeoutError(name)
        context.event_sink.emit(name)
    context.stop_event.wait(1)


def staggered_completed_worker(context):
    if context.worker_id == 1:
        deadline = time.monotonic() + 0.12
        while time.monotonic() < deadline:
            context.heartbeat()
            context.stop_event.wait(0.005)
    context.event_sink.emit("WORKLOAD_COMPLETE", roomGroupId=context.worker_id)
    if not context.wait_for_stop_with_heartbeat(1):
        raise TimeoutError("parent did not close the completed worker")


class RunnerRuntimeTests(unittest.TestCase):
    def setUp(self) -> None:
        if ParentRuntime is None:
            self.fail("parent/worker runtime module is absent")

    def test_room_groups_have_one_fixed_owner(self):
        assignments = assign_room_groups(room_group_count=5, worker_count=2)
        self.assertEqual((0, 1, 2), assignments[0].room_group_ids)
        self.assertEqual((3, 4), assignments[1].room_group_ids)
        self.assertEqual({0, 1, 2, 3, 4}, set(sum((a.room_group_ids for a in assignments), ())))

    def test_parent_observes_heartbeat_and_completes_state_machine(self):
        runtime = ParentRuntime(
            room_group_count=1,
            worker_count=1,
            worker_entry=quiet_worker,
            heartbeat_interval_seconds=0.02,
            heartbeat_timeout_seconds=0.2,
        )
        try:
            runtime.start(timeout_seconds=2)
            self.assertEqual(RunnerPhase.READY, runtime.phase)
            deadline = time.monotonic() + 1
            while not any(event["type"] == "HEARTBEAT" for event in runtime.events):
                self.assertLess(time.monotonic(), deadline)
                runtime.poll(timeout_seconds=0.05)
            runtime.begin_warmup()
            runtime.begin_measurement()
            runtime.begin_draining()
            runtime.complete()
            self.assertEqual(RunnerPhase.MEASUREMENT_COMPLETE, runtime.phase)
        finally:
            runtime.close()

    def test_worker_crash_propagates_without_hidden_restart(self):
        runtime = ParentRuntime(
            room_group_count=1,
            worker_count=1,
            worker_entry=crashing_worker,
            heartbeat_interval_seconds=0.02,
            heartbeat_timeout_seconds=1,
        )
        try:
            runtime.start(timeout_seconds=2)
            deadline = time.monotonic() + 2
            with self.assertRaises(WorkerCrashed):
                while True:
                    self.assertLess(time.monotonic(), deadline)
                    runtime.poll(timeout_seconds=0.05)
        finally:
            runtime.abort()
            runtime.close()

    def test_missing_worker_heartbeat_expires_attempt(self):
        runtime = ParentRuntime(
            room_group_count=1,
            worker_count=1,
            worker_entry=silent_worker,
            heartbeat_interval_seconds=0.01,
            heartbeat_timeout_seconds=0.05,
        )
        try:
            runtime.start(timeout_seconds=2)
            time.sleep(0.06)
            with self.assertRaises(WorkerHeartbeatExpired):
                runtime.poll()
        finally:
            runtime.abort()
            runtime.close()

    def test_bounded_ipc_reports_saturation(self):
        context = multiprocessing.get_context("spawn")
        queue = context.Queue(maxsize=1)
        try:
            sink = WorkerEventSink(queue, worker_id=0)
            sink.emit("READY")
            with self.assertRaises(IpcQueueSaturated):
                sink.emit("HEARTBEAT")
        finally:
            queue.close()
            queue.join_thread()

    def test_abort_is_terminal_and_stops_workers(self):
        runtime = ParentRuntime(
            room_group_count=1,
            worker_count=1,
            worker_entry=quiet_worker,
            heartbeat_interval_seconds=0.02,
        )
        runtime.start(timeout_seconds=2)
        runtime.begin_warmup()
        runtime.abort()
        self.assertEqual(RunnerPhase.ABORTED, runtime.phase)
        self.assertTrue(all(not process.is_alive() for process in runtime.processes))
        runtime.close()

    def test_forced_worker_termination_cannot_be_measurement_complete(self):
        runtime = ParentRuntime(
            room_group_count=1,
            worker_count=1,
            worker_entry=uncooperative_worker,
            heartbeat_interval_seconds=0.01,
            heartbeat_timeout_seconds=1,
            join_timeout_seconds=0.02,
        )
        try:
            runtime.start(timeout_seconds=2)
            runtime.begin_warmup()
            runtime.begin_measurement()
            runtime.begin_draining()
            with self.assertRaises(WorkerCrashed):
                runtime.complete()
            self.assertEqual(RunnerPhase.ABORTED, runtime.phase)
        finally:
            runtime.close()

    def test_parent_phase_transitions_are_visible_to_all_workers(self):
        runtime = ParentRuntime(
            room_group_count=1,
            worker_count=1,
            worker_entry=phase_observing_worker,
        )
        try:
            runtime.start(timeout_seconds=2)
            for transition, expected in (
                (runtime.begin_warmup, "WARMUP_SEEN"),
                (runtime.begin_measurement, "MEASUREMENT_SEEN"),
                (runtime.begin_draining, "DRAINING_SEEN"),
            ):
                transition()
                deadline = time.monotonic() + 1
                while not any(event["type"] == expected for event in runtime.events):
                    self.assertLess(time.monotonic(), deadline)
                    runtime.poll(timeout_seconds=0.05)
            runtime.complete()
        finally:
            runtime.close()

    def test_completed_worker_keeps_heartbeating_until_all_workers_complete(self):
        runtime = ParentRuntime(
            room_group_count=2,
            worker_count=2,
            worker_entry=staggered_completed_worker,
            heartbeat_interval_seconds=0.01,
            heartbeat_timeout_seconds=0.05,
        )
        try:
            runtime.start(timeout_seconds=2)
            completed = {
                event["workerId"]
                for event in runtime.events
                if event["type"] == "WORKLOAD_COMPLETE"
            }
            deadline = time.monotonic() + 1
            while completed != {0, 1}:
                self.assertLess(time.monotonic(), deadline)
                completed.update(
                    event["workerId"]
                    for event in runtime.poll(timeout_seconds=0.02)
                    if event["type"] == "WORKLOAD_COMPLETE"
                )
            runtime.begin_warmup()
            runtime.begin_measurement()
            runtime.begin_draining()
            runtime.complete()
            self.assertEqual(RunnerPhase.MEASUREMENT_COMPLETE, runtime.phase)
        finally:
            runtime.close()


if __name__ == "__main__":
    unittest.main()
