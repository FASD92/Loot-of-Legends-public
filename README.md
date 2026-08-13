# Loot of Legends — Evidence-first Game Server Portfolio

> C++20 server-authoritative multiplayer game server: non-blocking TCP/UDP,
> Linux `epoll`, per-room single-writer concurrency, Custom RUDP, durable
> settlement, and a reproducible load-validation harness.

이 저장소는 채용 검토용으로 선별한 public evidence mirror다. 검증 기준은 이 저장소의
Git-tracked tree이며, 운영 설정·raw evidence·secret은 포함하지 않는다.

## Problem

- 같은 Room 상태는 한 번에 한 worker만 변경해야 하지만, 서로 다른 Room은 병렬 실행되어야 한다.
- UDP gameplay에서 movement snapshot과 terminal combat/loot 결과는 서로 다른 신뢰성 정책이 필요하다.
- 패킷·명령·정산 재시도는 허용하되 damage·loot owner·asset을 중복 적용하면 안 된다.

## Architecture

```text
Unity Client
        ↓
non-blocking TCP/UDP sockets → Linux EpollReactor
├─ TCP control → authentication / lobby / battle load / final result
└─ UDP datagram → RUDP binding / ACK bitmap / bounded retransmission
                         ↓
                movement / combat / loot
                         ↓
        RoomCommandGateway → RoomExecutionCell → BattleInstance
                               single writer       server authority
                         ↓
        durable journal → publisher → Spring/MySQL inbox → assets
```

## What I built

- C++20 non-blocking TCP/UDP composition root와 Linux `epoll` event reactor
- sequence, `ack`, `ackBits`, CRC, bounded retry/expiry를 갖춘 Custom RUDP
- 같은 Room은 직렬화하고 다른 Room은 병렬 실행하는 `RoomExecutionCell`
- 이동·공격·루팅 결과를 서버에서 결정하는 authoritative battle domain
- 실행 판정과 evidence package 완결성을 독립 상태로 남기는 load validation harness
- 동일 settlement ID 재전송과 payload hash conflict를 구분하는 Spring/MySQL inbox

## Evidence — 직접 해결한 어려운 문제

| 문제 | 구현 근거 | 회귀 근거 |
| --- | --- | --- |
| C++20에서 TCP/UDP를 non-blocking event loop로 연결 | [`CMakeLists.txt`](CMakeLists.txt), [`ConfiguredGameServer.cpp`](game-server/app/composition-root/ConfiguredGameServer.cpp), [`EpollReactor.cpp`](game-server/platform/runtime-linux/src/EpollReactor.cpp) | [`TcpTransportTests.cpp`](tests/integration/session/TcpTransportTests.cpp), [`RuntimeLinuxReadinessTests.cpp`](tests/runtime-linux/RuntimeLinuxReadinessTests.cpp), [`ServerEntryTests.cpp`](tests/integration/server-entry/ServerEntryTests.cpp) |
| 같은 Room은 single-writer, 다른 Room은 병렬 | [`RoomExecutionCell.cpp`](game-server/modules/game-flow/src/execution/RoomExecutionCell.cpp), [`WorkerPool.cpp`](game-server/platform/runtime/src/WorkerPool.cpp) | [`RoomExecutionCellTests.cpp`](game-server/modules/game-flow/tests/RoomExecutionCellTests.cpp), [`RoomGatewayRaceTests.cpp`](tests/integration/lobby-room/RoomGatewayRaceTests.cpp) |
| UDP 손실·중복과 gameplay 중복 적용을 분리 | [`RudpHeader.cpp`](game-server/platform/transport-rudp/src/RudpHeader.cpp), [`ReliableQueue.cpp`](game-server/platform/transport-rudp/src/ReliableQueue.cpp), [`AttackResultStore.cpp`](game-server/modules/battle/src/application/AttackResultStore.cpp) | [`RudpDeliveryTests.cpp`](tests/transport-rudp/RudpDeliveryTests.cpp), [`CombatFlowTests.cpp`](tests/integration/combat/CombatFlowTests.cpp) |
| client intent만 받고 위치·damage·loot owner는 서버가 결정 | [`BattleInstance.cpp`](game-server/modules/battle/src/domain/BattleInstance.cpp), [`LootHandler.cpp`](game-server/modules/battle/src/application/LootHandler.cpp) | [`MovementTests.cpp`](tests/integration/movement/MovementTests.cpp), [`CombatOutcomeTests.cpp`](tests/integration/combat/CombatOutcomeTests.cpp), [`LootClaimTests.cpp`](game-server/modules/battle/tests/LootClaimTests.cpp) |
| 응답 유실·재시도에도 asset을 한 번만 반영 | [`SettlementApplication.java`](meta-server/src/main/java/com/fasd92/lootoflegends/meta/settlement/application/SettlementApplication.java), [`JdbcSettlementInbox.java`](meta-server/src/main/java/com/fasd92/lootoflegends/meta/platform/mysql/JdbcSettlementInbox.java) | [`SettlementPublisherTests.cpp`](tests/integration/settlement/SettlementPublisherTests.cpp), [`SettlementAcceptanceTest.java`](meta-server/src/test/java/com/fasd92/lootoflegends/meta/settlement/SettlementAcceptanceTest.java) |
| 실행 결과와 evidence package 완결성을 한 상태로 뭉개지 않기 | [`runtime.py`](tools/load/loot_load/runner/runtime.py), [`classifier.py`](tools/load/loot_load/closeout/classifier.py), [`package.py`](tools/load/loot_load/evidence/package.py) | [`test_runner_runtime.py`](tools/load/tests/test_runner_runtime.py), [`test_closeout_classifier.py`](tools/load/tests/test_closeout_classifier.py), [`test_package_closeout.py`](tools/load/tests/test_package_closeout.py) |

Custom RUDP의 transport ACK와 application `CommandId` 멱등성은 별도 계층이다.
Movement는 newest-only이고, attack/loot은 재정렬된 고유 command를 허용한다.
**ordered delivery나 exactly-once network delivery를 주장하지 않는다.**

## Measured results

2026-08-13에 이 public branch에서 다시 실행한 결과다.

<!-- portfolio-test-count: ctest=46 load-unittest=47 -->

| 검증 | 결과 | 의미 |
| --- | --- | --- |
| Architecture contract | **PASS** | CMake target graph와 module include policy, 0 findings |
| C++ configure/build + CTest | **46/46 PASS** | tracked CTest 등록 전체의 local 실행 |
| Linux public CI | **PASS** | Ubuntu CMake/build/CTest에서 Linux `epoll` 경로 검증 |
| Load harness unit/contract | **47/47 PASS** | runner, workload, classifier, sanitizer, package 검증 |
| Local boundary fixture | **COMPLETE** | 실제 game-server에 10명·1 Room·2 loot cycle; `NOT_CLASSIFIED` |
| Meta locked Gradle check | **BUILD SUCCESSFUL** | Java 21 + MySQL/Redis Testcontainers |
| Official capacity | **NOT PROVEN** | qualifying 공개 run이 없어 동시접속·TPS·p95/p99 수치를 주장하지 않음 |

실행 환경과 미실행 항목은 [verification report](docs/verification.md), 공개 근거의 한계는
[public evidence limitations](docs/public-evidence-limitations.md)에 분리했다.

## How to verify

필수 도구: CMake 3.28+, Ninja, C++20 compiler, libcurl/OpenSSL, Python 3.12+.
Meta 검증에는 Java 21과 실행 중인 Docker가 추가로 필요하다.

```bash
python3 -m unittest scripts/public/test_verify_portfolio_claims.py -v
python3 scripts/public/verify_portfolio_claims.py

cmake --preset dev-debug
cmake --build --preset dev-debug --parallel 4
ctest --preset dev-debug --output-on-failure

python3 -m unittest discover -s tools/load/tests -p 'test_*.py' -v

cd meta-server
./gradlew --no-daemon --dependency-verification strict clean check
```

실제 game-server TCP/UDP/RUDP 경계를 통과하는 비정량 10-player fixture는
[`tools/load/README.md`](tools/load/README.md)에 있다. `fixtureOnly=true`인 기능 검증이며
capacity claim에는 사용할 수 없다.

더 읽기: [architecture](docs/architecture.md) ·
[adversarial recruiter audit](docs/recruiter-audit.md) ·
[verification](docs/verification.md) ·
[limitations](docs/public-evidence-limitations.md)
