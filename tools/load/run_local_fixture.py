#!/usr/bin/env python3
"""Run the non-qualifying local Functional 10P fixture through the real Game entry point."""

from __future__ import annotations

import argparse
import asyncio
import functools
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.parse
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.load.loot_load.client.session import BoundaryGameClient  # noqa: E402
from tools.load.fixture_support import MetaFixture, start_server, stop_process  # noqa: E402
from tools.load.loot_load.evidence.contracts import (  # noqa: E402
    document_digest,
    validate_document,
)
from tools.load.loot_load.evidence.fact_writer import (  # noqa: E402
    JsonlFactWriter,
    write_json_atomic,
)
from tools.load.loot_load.evidence.package import close_package, verify_package  # noqa: E402
from tools.load.loot_load.protocol.meta import MetaHttpClient  # noqa: E402
from tools.load.loot_load.runner.runtime import (  # noqa: E402
    ParentRuntime,
    WorkerContext,
)
from tools.load.loot_load.workload.loot_race import (  # noqa: E402
    LootRaceCoordinator,
    ScheduledCommand,
)


def _opaque(seed: int, participant: int, purpose: str) -> str:
    import base64

    digest = hashlib.sha256(f"{seed}:{participant}:{purpose}".encode()).digest()
    return base64.urlsafe_b64encode(digest).decode().rstrip("=")


async def _admit_participant(
    meta: MetaHttpClient,
    *,
    participant: int,
    seed: int,
    game_host: str,
    tcp_port: int,
    rudp_port: int,
) -> BoundaryGameClient:
    state = _opaque(seed, participant, "state")
    started = await meta.start_desktop_auth(
        state=state,
        code_challenge=_opaque(seed, participant, "challenge"),
        loopback_redirect_uri=f"http://127.0.0.1:{51000 + participant}/callback",
    )
    query = urllib.parse.parse_qs(urllib.parse.urlsplit(started["authorizationUrl"]).query)
    handoff = query.get("handoff", [None])[0]
    if not isinstance(handoff, str):
        raise ValueError("fixture authorization URL omitted the handoff code")
    session = await meta.exchange_desktop_auth(
        handoff_code=handoff,
        state=state,
        code_verifier=_opaque(seed, participant, "verifier"),
    )
    return await BoundaryGameClient.connect(
        meta=meta,
        meta_session=session["metaSession"],
        game_host=game_host,
        tcp_port=tcp_port,
        rudp_port=rudp_port,
        timeout_seconds=5,
    )


async def _dispatch(
    clients: list[BoundaryGameClient],
    commands: list[ScheduledCommand],
    writer: JsonlFactWriter,
    *,
    participant_offset: int = 0,
    started_at: float | None = None,
    measurement: bool = False,
) -> None:
    for command in commands:
        writer.append(
            {
                "type": "COMMAND_PLANNED",
                "atMillis": command.at_ms,
                "participantIndex": participant_offset + command.participant_index,
                "channel": command.channel,
                "name": command.name,
                "applicationAttempt": command.application_attempt,
                "measurement": measurement,
                "fields": command.fields,
            }
        )
        client = clients[command.participant_index]
        if command.channel == "TCP":
            await client.tcp.send(command.name, command.fields)
        elif command.name == "MoveIntent":
            await client.rudp.send_unreliable(command.name, command.fields)
        else:
            await client.rudp.send_reliable(command.name, command.fields)
        emitted_at = time.monotonic()
        writer.append(
            {
                "type": "COMMAND_EMITTED",
                "atMillis": command.at_ms,
                "participantIndex": participant_offset + command.participant_index,
                "channel": command.channel,
                "name": command.name,
                "applicationAttempt": command.application_attempt,
                "measurement": measurement,
                "schedulerSlipMillis": (
                    0
                    if started_at is None
                    else max(0, int((emitted_at - started_at) * 1000) - command.at_ms)
                ),
                "fields": command.fields,
            }
        )


def _record_transport_events(
    clients: list[BoundaryGameClient],
    writer: JsonlFactWriter,
    *,
    participant_offset: int,
    at_ms: int,
    measurement: bool,
) -> None:
    for participant, client in enumerate(clients):
        for event in client.rudp.drain_transport_events():
            writer.append(
                {
                    "type": "TRANSPORT_OBSERVATION",
                    "atMillis": at_ms,
                    "participantIndex": participant_offset + participant,
                    "kind": event.kind,
                    "transportSequence": event.sequence,
                    "transportAttempt": event.attempt,
                    "measurement": measurement,
                }
            )


async def _receive_one(
    clients: list[BoundaryGameClient], timeout_seconds: float
) -> list[tuple[int, str, Any]]:
    tasks: dict[asyncio.Task[Any], tuple[int, str]] = {}
    for participant, client in enumerate(clients):
        tasks[asyncio.create_task(client.tcp.next_event(timeout_seconds=timeout_seconds))] = (
            participant,
            "TCP",
        )
        tasks[asyncio.create_task(client.rudp.next_message(timeout_seconds=timeout_seconds))] = (
            participant,
            "RUDP",
        )
    done, pending = await asyncio.wait(
        tasks, timeout=timeout_seconds, return_when=asyncio.FIRST_COMPLETED
    )
    for task in pending:
        task.cancel()
    if pending:
        await asyncio.gather(*pending, return_exceptions=True)
    received: list[tuple[int, str, Any]] = []
    for task in done:
        try:
            message = task.result()
        except TimeoutError:
            continue
        participant, channel = tasks[task]
        received.append((participant, channel, message))
    return received


async def _run_fixture_room(
    context: WorkerContext,
    *,
    profile: dict[str, Any],
    meta_base_url: str,
    game_host: str,
    tcp_port: int,
    rudp_port: int,
    fact_path: Path,
) -> tuple[int, tuple[int, ...]]:
    meta = MetaHttpClient(
        meta_base_url,
        allow_insecure_loopback=True,
    )
    participant_offset = 0
    coordinator = LootRaceCoordinator(
        profile["seed"],
        required_cycles=profile["requiredCyclesPerRoom"],
    )
    clients: list[BoundaryGameClient] = []
    with JsonlFactWriter(fact_path, stream_id=f"worker-{context.worker_id}") as writer:
        try:
            for participant in range(profile["participantsPerRoom"]):
                client = await _admit_participant(
                    meta,
                    participant=participant,
                    seed=profile["seed"],
                    game_host=game_host,
                    tcp_port=tcp_port,
                    rudp_port=rudp_port,
                )
                clients.append(client)
                coordinator.on_tcp(participant, client.welcome, 0)
                coordinator.on_rudp(participant, client.bind_accepted, 0)
                writer.append(
                    {
                        "type": "BOUNDARY_CONNECTED",
                        "participantIndex": participant_offset + participant,
                        "sessionId": client.identity.session_id,
                        "sessionGeneration": client.identity.session_generation,
                        "nickname": client.identity.nickname,
                    }
                )
                context.heartbeat()

            context.event_sink.emit("WORKLOAD_READY", roomGroupId=0)
            while not context.warmup_event.is_set():
                if context.stop_event.is_set():
                    raise RuntimeError("workload stopped before warmup")
                context.heartbeat()
                await asyncio.sleep(0.05)
            started = time.monotonic()
            deadline = started + profile["overallDeadlineSeconds"]
            await _dispatch(
                clients,
                coordinator.take_commands(),
                writer,
                participant_offset=participant_offset,
                started_at=started,
                measurement=context.measurement_event.is_set(),
            )
            _record_transport_events(
                clients,
                writer,
                participant_offset=participant_offset,
                at_ms=0,
                measurement=context.measurement_event.is_set(),
            )
            while not coordinator.complete:
                if time.monotonic() >= deadline:
                    raise TimeoutError("Loot Race room exceeded its fixed deadline")
                now_ms = int((time.monotonic() - started) * 1000)
                coordinator.tick(now_ms)
                await _dispatch(
                    clients,
                    coordinator.take_commands(),
                    writer,
                    participant_offset=participant_offset,
                    started_at=started,
                    measurement=(
                        context.measurement_event.is_set()
                        and not context.draining_event.is_set()
                    ),
                )
                for participant, channel, message in await _receive_one(clients, 0.02):
                    writer.append(
                        {
                            "type": "SERVER_OBSERVATION",
                            "atMillis": now_ms,
                            "participantIndex": participant_offset + participant,
                            "channel": channel,
                            "name": message.name,
                            "measurement": (
                                context.measurement_event.is_set()
                                and not context.draining_event.is_set()
                            ),
                            "fields": message.fields,
                        }
                    )
                    if channel == "TCP":
                        coordinator.on_tcp(participant, message, now_ms)
                    else:
                        coordinator.on_rudp(participant, message, now_ms)
                _record_transport_events(
                    clients,
                    writer,
                    participant_offset=participant_offset,
                    at_ms=now_ms,
                    measurement=(
                        context.measurement_event.is_set()
                        and not context.draining_event.is_set()
                    ),
                )
                context.heartbeat()

            for participant, client in enumerate(clients):
                for observation in range(3):
                    collection = await client.get_public_meta_json("/api/v1/collection")
                    writer.append(
                        {
                            "type": "COLLECTION_OBSERVATION",
                            "participantIndex": participant_offset + participant,
                            "observation": observation + 1,
                            "fields": collection,
                        }
                    )
            writer.append(
                {
                    "type": "FIXTURE_WORKLOAD_COMPLETE",
                    "roomGroupId": 0,
                    "roomId": coordinator.room_id,
                    "battleInstanceIds": list(coordinator.completed_battle_ids),
                }
            )
            return coordinator.room_id or 0, coordinator.completed_battle_ids
        finally:
            await asyncio.gather(*(client.close() for client in clients), return_exceptions=True)


def _fixture_worker(
    context: WorkerContext,
    *,
    profile: dict[str, Any],
    meta_base_url: str,
    game_host: str,
    tcp_port: int,
    rudp_port: int,
    fact_path: Path,
) -> None:
    room_id, battle_ids = asyncio.run(
        _run_fixture_room(
            context,
            profile=profile,
            meta_base_url=meta_base_url,
            game_host=game_host,
            tcp_port=tcp_port,
            rudp_port=rudp_port,
            fact_path=fact_path,
        )
    )
    context.event_sink.emit(
        "WORKLOAD_COMPLETE",
        roomId=room_id,
        battleInstanceIds=list(battle_ids),
    )
    if not context.wait_for_stop_with_heartbeat(10):
        raise TimeoutError("parent did not close the completed fixture worker")


def _require_fixture_profile(profile: dict[str, Any]) -> None:
    errors = validate_document("workload-profile", profile)
    if errors:
        raise ValueError("profile is invalid: " + "; ".join(errors))
    canonical_names = {
        "release-functional-10p": "release-functional-10p-v1.json",
        "release-smoke-10p": "release-smoke-10p-v1.json",
    }
    canonical_name = canonical_names.get(profile.get("profileId"))
    if canonical_name is None:
        raise ValueError("local fixture mode accepts only Functional/Smoke 10P profiles")
    canonical = json.loads((ROOT / "tools/load/profiles" / canonical_name).read_bytes())
    if profile != canonical:
        raise ValueError("local fixture mode accepts only exact Functional/Smoke 10P profiles")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _source_state() -> tuple[str, bool]:
    identity_path = ROOT / "build-identity.json"
    if identity_path.is_file():
        identity = json.loads(identity_path.read_bytes())
        if not isinstance(identity, dict) or set(identity) != {
            "sourceSha",
            "sourceDirty",
        }:
            raise ValueError("build identity must contain only sourceSha and sourceDirty")
        sha = identity["sourceSha"]
        dirty = identity["sourceDirty"]
        if (
            not isinstance(sha, str)
            or len(sha) != 40
            or any(character not in "0123456789abcdef" for character in sha)
            or not isinstance(dirty, bool)
        ):
            raise ValueError("build identity is invalid")
        return sha, dirty

    sha = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
    ).strip()
    dirty = bool(
        subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True)
    )
    return sha, dirty


def run_local_fixture(profile_path: Path, server: Path, output: Path, run_id: str) -> None:
    if output.exists():
        raise FileExistsError(output)
    if not server.is_file() or not os.access(server, os.X_OK):
        raise ValueError("fixture server must be an executable file")
    if (
        not 1 <= len(run_id) <= 128
        or not run_id[0].isalnum()
        or any(not (character.isalnum() or character in "._-") for character in run_id)
    ):
        raise ValueError("run_id violates the evidence identifier contract")
    if profile_path.stat().st_size > 64 * 1024:
        raise ValueError("profile exceeds 64 KiB")
    profile = json.loads(profile_path.read_bytes())
    if not isinstance(profile, dict):
        raise ValueError("profile must be an object")
    _require_fixture_profile(profile)
    output.mkdir(parents=True, mode=0o700)
    write_json_atomic(output / "manifest" / "profile.json", profile)

    source_sha, source_dirty = _source_state()
    server_process = None
    completion_event: dict[str, Any] | None = None
    with tempfile.TemporaryDirectory(prefix="lol-load-functional-") as temporary:
        try:
            with MetaFixture() as meta_fixture:
                server_process, tcp_port, rudp_port = start_server(server, Path(temporary))
                entry = functools.partial(
                    _fixture_worker,
                    profile=profile,
                    meta_base_url=meta_fixture.base_url,
                    game_host="127.0.0.1",
                    tcp_port=tcp_port,
                    rudp_port=rudp_port,
                    fact_path=output / "raw" / "worker-0.jsonl",
                )
                runtime = ParentRuntime(
                    room_group_count=1,
                    worker_count=1,
                    worker_entry=entry,
                    queue_capacity=128,
                    heartbeat_interval_seconds=1,
                    heartbeat_timeout_seconds=5,
                )
                with JsonlFactWriter(
                    output / "raw" / "parent-facts.jsonl", stream_id="parent"
                ) as writer:
                    try:
                        runtime.start(timeout_seconds=10)
                        runtime.begin_warmup()
                        runtime.begin_measurement()
                        deadline = time.monotonic() + profile["overallDeadlineSeconds"]
                        while time.monotonic() < deadline:
                            events = runtime.poll(timeout_seconds=0.1)
                            for event in events:
                                writer.append({"type": "WORKER_EVENT", "event": event})
                                if event.get("type") == "WORKLOAD_COMPLETE":
                                    completion_event = event
                            if completion_event is not None:
                                break
                        if completion_event is None:
                            raise TimeoutError("fixture worker did not complete")
                        runtime.begin_draining()
                        runtime.complete()
                        writer.append({"type": "MEASUREMENT_COMPLETE"})
                    finally:
                        runtime.close()
            server_code = stop_process(server_process)
            server_process = None
            if server_code != 0:
                raise RuntimeError(f"fixture Game server exit code is {server_code}")
        finally:
            stop_process(server_process)

    fixture_environment = {
        "schemaVersion": 1,
        "runId": run_id,
        "fixtureOnly": True,
        "qualificationEligible": False,
        "evaluationStatus": "NOT_CLASSIFIED",
        "sourceSha": source_sha,
        "sourceDirty": source_dirty,
        "serverArtifactDigest": _sha256(server),
        "workloadProfileDigest": document_digest(profile),
        "participantCount": 10,
        "roomCount": 1,
        "roomId": completion_event["roomId"],
        "battleInstanceIds": completion_event["battleInstanceIds"],
        "completedCyclesPerRoom": len(completion_event["battleInstanceIds"]),
    }
    write_json_atomic(output / "manifest" / "fixture-environment.json", fixture_environment)
    package = close_package(
        output,
        output / "package",
        run_id=run_id,
        required_paths=(
            "manifest/fixture-environment.json",
            "manifest/profile.json",
            "raw/parent-facts.jsonl",
            "raw/worker-0.jsonl",
        ),
    )
    if package["packageStatus"] != "COMPLETE" or verify_package(output / "package")[
        "packageStatus"
    ] != "COMPLETE":
        raise RuntimeError("fixture evidence package did not close completely")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--run-id")
    parser.add_argument(
        "--fixture-server",
        type=Path,
        required=True,
        help="test-enabled lol_game_server; this mode never creates qualifying evidence",
    )
    args = parser.parse_args()
    run_id = args.run_id or args.output.name
    try:
        run_local_fixture(args.profile, args.fixture_server.resolve(), args.output, run_id)
    except KeyboardInterrupt:
        print(f"ABORTED {args.output}", file=sys.stderr)
        return 3
    except Exception as error:
        print(f"RUN ERROR {type(error).__name__}: {error}", file=sys.stderr)
        return 2
    print(f"FIXTURE COMPLETE {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
