# Evidence-backed architecture

이 문서는 public tree에서 직접 따라갈 수 있는 runtime 경계만 설명한다. 계획 문서나
private 운영 자료를 근거로 사용하지 않는다.

## Runtime flow

```text
Unity
  ├─ TCP control path
  │    authentication → lobby/room → battle load → final result
  └─ UDP gameplay path
       RUDP bind → movement / attack / claim-loot → ACK/retransmission
                            │
                            v
                 RoomCommandGateway
                            │
                            v
                 RoomExecutionCell
              bounded dual-lane mailbox
              one active run per Room
                            │
                            v
                    BattleInstance
          server tick / position / range / cooldown
          damage / drop state / first accepted owner
                            |
          일반 변경은 RAM 기록 후 즉시 결과 전송
       중요 사건은 누적 기록 + checkpoint data sync
                  battle replay / restore
                            |
         durable journal → SettlementPublisher
                            │
                            v
       Spring settlement inbox → transactional asset apply
```

제품 연결은 [`ConfiguredGameServer.cpp`](../game-server/app/composition-root/ConfiguredGameServer.cpp)에서
확인할 수 있다. TCP/UDP socket, `RoomCommandGateway`, RUDP movement/combat flow,
settlement storage/publisher가 이 composition root에서 실제로 조립된다.

## Async/event-driven networking

- [`TcpConnection.hpp`](../game-server/platform/transport-tcp/include/lol/transport/tcp/TcpConnection.hpp)는
  pre-auth framing, timeout/rate/byte budget, outbound backpressure와 non-blocking I/O 상태를 분리한다.
- [`EpollReactor.cpp`](../game-server/platform/runtime-linux/src/EpollReactor.cpp)는 Linux에서
  `epoll`, `eventfd`, `timerfd`로 socket/wake/timer readiness를 전달한다.
- Linux가 아닌 POSIX 개발 환경에서는 composition root가 `poll` fallback을 사용한다.
- [`RudpBindingRegistry.cpp`](../game-server/platform/transport-rudp/src/RudpBindingRegistry.cpp)는
  TCP로 인증된 session generation과 UDP endpoint/transport epoch를 결합한다.

`epoll` 구현의 존재와 Linux 실제 실행은 구분한다. local macOS CTest는 fallback 경로를
사용하며, public CI의 Ubuntu job이 Linux build/CTest를 담당한다.

## Per-room single-writer concurrency

[`RoomExecutionCell.hpp`](../game-server/modules/game-flow/src/execution/RoomExecutionCell.hpp)는
외부 명령 224개와 control 명령 32개의 bounded mailbox를 소유한다. 두 lane은 각자의 FIFO를
유지하면서 공통 admission ordinal로 merge된다.

Cell mutex 아래 `scheduled_`를 전이한 한 run만 `WorkerPool`에 제출하므로 같은 Cell의
`maximumConcurrentRuns`는 1이다. 한 turn은 command 수와 wall-time budget을 가진 뒤 남은
작업을 reschedule한다. 서로 다른 Cell은 같은 worker pool에서 동시에 실행할 수 있다.

이를 고정하는 테스트:

- [`RoomExecutionCellTests.cpp`](../game-server/modules/game-flow/tests/RoomExecutionCellTests.cpp):
  one-cell single active worker, two-cell parallelism, mailbox saturation, lane merge
- [`WorkerPoolTests.cpp`](../game-server/platform/runtime/tests/WorkerPoolTests.cpp):
  bounded submit, accepted-task drain, stop lifecycle
- [`RoomGatewayRaceTests.cpp`](../tests/integration/lobby-room/RoomGatewayRaceTests.cpp):
  gateway/session/Room 경쟁 경계

## Same-host battle continuity

[`FlightRecorder.cpp`](../game-server/modules/battle-continuity/src/FlightRecorder.cpp)는 domain admission을 통과한 결정과 state hash를 RAM에 누적합니다. 일반 이동과 비치명적 공격은 저장 요청 없이 결과를 전송합니다. 전투 시작과 상태 전환 및 몬스터 처치와 전리품 변경 및 전투 종료에서는 누적 기록과 전체 checkpoint를 [`ContinuityStorage.cpp`](../game-server/platform/battle-continuity-storage/src/ContinuityStorage.cpp)에 append하고 data sync합니다. 중요 결과는 저장 완료 후 전송합니다.

주기 저장은 없습니다. 장애가 나면 마지막 중요 저장 이후의 일반 변경은 모두 되돌아갈 수 있습니다. 자세한 기준은 [`ADR-0019`](adr/ADR-0019-critical-event-battle-persistence.md)와 [`critical-event-policy-v1.json`](../contracts/battle-continuity/critical-event-policy-v1.json)에 있습니다.

재시작하면 [`BattleReplay.cpp`](../game-server/modules/battle-continuity/src/BattleReplay.cpp)가 checkpoint 이후 기록을 실제 `BattleInstance` 규칙으로 재생합니다. [`BattleContinuityRecovery.cpp`](../game-server/modules/game-flow/src/BattleContinuityRecovery.cpp)는 복구한 Room과 Session 및 Battle을 listener가 열리기 전에 설치합니다. 계약은 [`battle-continuity-v1.md`](../contracts/battle-continuity/battle-continuity-v1.md)가 소유합니다.

이 경로는 같은 host와 endpoint 및 local durable filesystem을 전제로 합니다. 다중 host 장애 전환이나 storage replication은 제공하지 않습니다.

## Custom RUDP: transport reliability is not gameplay idempotency

| 계층 | 구현된 정책 | 근거 |
| --- | --- | --- |
| Datagram | 48-byte header, CRC, session generation, transport epoch, sequence/ACK/ACK bitmap | [`RudpHeader.cpp`](../game-server/platform/transport-rudp/src/RudpHeader.cpp) |
| Receive window | duplicate/reordered/too-old 판정, 최근 32개 ACK bitmap | [`RudpPeer.cpp`](../game-server/platform/transport-rudp/src/RudpPeer.cpp) |
| Reliable send | entry/byte/application lane 제한, 최대 5회 전송, backoff, 5초 expiry | [`ReliableQueue.cpp`](../game-server/platform/transport-rudp/src/ReliableQueue.cpp) |
| Movement | newer action sequence만 수용하는 newest-only state | [`RudpMovementFlow.cpp`](../game-server/app/composition-root/RudpMovementFlow.cpp) |
| Attack/Loot | reordered unique command 허용, `(session, generation, battle, CommandId)` replay/conflict 판정 | [`RudpCombatFlow.cpp`](../game-server/app/composition-root/RudpCombatFlow.cpp), [`LootResultStore.cpp`](../game-server/modules/battle/src/application/LootResultStore.cpp) |

따라서 이 구현은 ACK·재전송·중복 억제를 제공하지만 transport ordered delivery를 제공한다고
쓰지 않는다. `CommandId`가 같은 동일 payload는 이전 terminal result를 replay하고, 같은 ID의
다른 payload는 conflict다.

## Server-authoritative gameplay

클라이언트는 movement direction, attack target hint, drop ID 같은 intent만 보낸다.
[`BattleInstance.cpp`](../game-server/modules/battle/src/domain/BattleInstance.cpp)가 session/battle
identity, generation, action freshness, rate limit, server tick, arena bounds, attack range,
cooldown, damage와 terminal outcome을 판정한다.

[`LootApi.hpp`](../game-server/modules/battle/include/lol/battle/LootApi.hpp)의 claim command에는
client position, item ID/value/quantity, eligibility, owner가 없다. [`LootHandler.cpp`](../game-server/modules/battle/src/application/LootHandler.cpp)가
서버가 보유한 position과 drop state로 range와 first accepted owner를 결정한다.

## Durable settlement and Java idempotency

Game server는 battle result를 durable journal에 append한 뒤
[`SettlementPublisher.cpp`](../game-server/modules/settlement/src/application/SettlementPublisher.cpp)로
Meta에 publish한다. Meta는 다음 두 단계를 분리한다.

1. [`SettlementApplication.java`](../meta-server/src/main/java/com/fasd92/lootoflegends/meta/settlement/application/SettlementApplication.java)가
   settlement ID를 row lock/primary key로 보호한다. 같은 ID와 hash는 기존 status를 반환하고,
   다른 hash는 conflict다.
2. [`SettlementApplyApplication.java`](../meta-server/src/main/java/com/fasd92/lootoflegends/meta/settlement/application/SettlementApplyApplication.java)가
   pending row를 `FOR UPDATE SKIP LOCKED`로 가져와 asset mutation과 `APPLIED` 전이를 하나의
   Spring transaction에서 수행한다.

[`SettlementAcceptanceTest.java`](../meta-server/src/test/java/com/fasd92/lootoflegends/meta/settlement/SettlementAcceptanceTest.java)는
MySQL Testcontainers에서 replay/conflict, apply-once, update 실패 rollback과 retry를 검증한다.

## Load/capacity validation boundary

[`runtime.py`](../tools/load/loot_load/runner/runtime.py)는 bounded worker, heartbeat, phase와 abort를
관리한다. [`classifier.py`](../tools/load/loot_load/closeout/classifier.py)는 실행을 `ABORTED`,
`INVALID`, `FAIL`, `PASS`로 판정하고 evidence의 `packageStatus`를 독립 필드로 기록한다.
[`package.py`](../tools/load/loot_load/evidence/package.py)는 raw/sanitized inventory, no-clobber,
SHA-256과 공개 leak scan을 검증한다. 따라서 `evaluationStatus=PASS`만으로 package 완결성을
주장하지 않고 `packageStatus`를 함께 확인해야 한다.

현재 공개 제품 코드는 qualifying capacity 실행을 새로 하지 않았습니다. [5,250세션 기록](cases/capacity-5250.md)은 더 이른 진단 source의 60분 실행이며 현재 build의 부하 결과로 바꾸지 않습니다. harness 구현과 기능 검증도 capacity 달성 수치가 아닙니다.
