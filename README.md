# Loot of Legends

> 클라이언트 입력을 신뢰하지 않고, 서버가 Room 상태와 루팅 소유권을 최종 판정하는 C++ 실시간 게임 서버 포트폴리오입니다.

`C++17` · `CMake` · `POSIX/BSD Sockets` · `TCP` · `GoogleTest` · `Server Authoritative` · `Debug CLI`

확장 중: `Custom RUDP` · `Room Actor / WorkerPool` · `Spring Boot Meta Server` · `MySQL / Redis Settlement`

---

## 목차

- [30초 요약](#30초-요약)
- [검증 완료된 핵심](#검증-완료된-핵심)
- [확장 중인 영역](#확장-중인-영역)
- [현재 한계 요약](#현재-한계-요약)
- [실행 / 테스트](#실행--테스트)
- [Debug CLI 시연](#debug-cli-시연)
- [코드 읽기 가이드](#코드-읽기-가이드)
- [자세한 문서](#자세한-문서)

---

## 30초 요약

- 2인 Room 기반 TCP gameplay loop를 구현했습니다.
- 서버가 Room, Monster, Drop, Loot, Inventory, SettlementResult 상태를 최종 판정합니다.
- Debug CLI로 Unity 없이 접속, Room 생성, Ready, 몬스터 처치, 루팅 경합, 정산 흐름을 재현할 수 있습니다.
- 동일 Drop 경합, 중복 클릭, 무게 제한, 반복 `finish_session` 요청을 테스트로 검증합니다.
- RUDP, Room Actor / WorkerPool, Spring Meta 정산은 후속 확장을 위한 기반 구현과 테스트 세로 슬라이스로 포함되어 있습니다.

이 프로젝트의 중심은 게임 화면이 아니라 서버 권한 판정 구조입니다. 클라이언트는 요청을 보낼 뿐이고, 실제 상태 전이와 최종 결과는 서버가 결정합니다.

```text
여러 플레이어가 같은 Drop을 클릭한다.
        ↓
서버가 Room / Drop / Inventory 상태로 승자를 확정한다.
        ↓
승자에게 LootResolved + InventorySnapshot을 보낸다.
패자에게 LootRejected를 보낸다.
        ↓
세션 종료 시 SettlementResult로 정산 경계를 넘긴다.
```

---

## 검증 완료된 핵심

| 영역 | 검증 수준 | 대표 파일 |
| --- | --- | --- |
| TCP Room / Loot 서버 | production 기준 gameplay 경로에서 Room 생성, Ready, Monster, Drop, ClickLoot, Inventory 흐름 검증 | `tests/core/RoomManagerTests.cpp`, `tests/core/ServerRoomIntegrationTests.cpp` |
| Debug CLI 시연 | 서버 상태 전이를 Unity 없이 명령 단위로 재현 | `client/debug_cli/`, `tests/client/DebugCliCommandTests.cpp` |
| SettlementResult 계약 | C++ 서버에서 정산 payload 생성과 반복 요청 멱등 응답 검증 | `tests/core/RoomManagerTests.cpp`, `tests/core/ServerRoomIntegrationTests.cpp` |
| TCP packet / session | packet framing, partial read, listener lifecycle, session id 발급 검증 | `tests/core/TcpPacketTests.cpp`, `tests/core/TcpPacketReaderTests.cpp`, `tests/core/SessionManagerTests.cpp` |

핵심 불변식:

- 한 세션은 동시에 하나의 Room에만 속합니다.
- Room 멤버가 모두 Ready 상태가 되었을 때 `BattleStart`는 한 번만 발생합니다.
- 동일 Drop은 한 번만 소유권이 확정됩니다.
- 이미 획득된 Drop을 다시 클릭해도 기존 소유자는 바뀌지 않습니다.
- 무게 제한 초과 루팅은 Drop 소유권을 확정하지 않습니다.
- 반복 `finish_session` 요청은 같은 서버 프로세스 안에서 동일한 `SettlementResult` payload를 반환합니다.

---

## 확장 중인 영역

| 영역 | 현재 단계 | 아직 남은 것 |
| --- | --- | --- |
| Spring Meta Server | 별도 모듈에서 정산 API, internal token 검증, MySQL transaction/idempotency 테스트 세로 슬라이스 검증 | C++ 서버의 runtime HTTP 연동 |
| Custom RUDP | ACK window, retransmission, Hello binding, InputCommand `cmdSeq` gate, Reliable Ordered event queue/payload/duplicate guard 검증 | 실제 UDP datagram 송신, ACK server-loop consume, gameplay transport 전환 |
| Room Actor / WorkerPool | `RoomEvent`, bounded queue, dispatcher, actor, worker, metrics primitive와 TCP inline actor pump regression 검증 | production `Core::Server` WorkerPool thread 연결 |
| Unity / Stress | 아직 public repo의 기준 시연 수단은 Debug CLI | Unity Thin Client, 100-client stress/soak test |

확장 영역은 현재 핵심 TCP gameplay path를 대체하지 않습니다. 먼저 도메인 불변식을 TCP와 테스트로 고정하고, RUDP/Actor/Meta는 통합 경계를 분리해서 확장하고 있습니다.

---

## 현재 한계 요약

- TCP gameplay path는 현재 2인 Room 기준입니다.
- Spring Meta Server는 별도 테스트 세로 슬라이스이며 C++ runtime HTTP 연동은 후속 범위입니다.
- RUDP는 protocol/unit layer와 Reliable Ordered event queue 중심이며 gameplay transport 전환은 후속 범위입니다.
- WorkerPool은 foundation/regression 단계이며 production thread 연결은 후속 범위입니다.
- Unity Thin Client와 100-client stress/soak test는 아직 예정입니다.

---

## 실행 / 테스트

요구 환경:

- CMake 3.20+
- C++17 compiler
- POSIX/BSD socket 사용 가능 환경
- GoogleTest
- Java 21
- Docker 실행 가능 환경

C++ 서버/CLI 빌드:

```bash
cmake -S . -B build
cmake --build build
```

서버 실행:

```bash
./build/server/lol_server 40000
```

Debug CLI 실행:

```bash
./build/client/lol_debug_cli
```

C++ 테스트:

```bash
ctest --test-dir build --output-on-failure
```

Meta Server 테스트:

```bash
cd meta-server
./gradlew test
```

---

## Debug CLI 시연

```text
connect A 127.0.0.1 40000
connect B 127.0.0.1 40000

A create_room
B join_room <roomId>

A ready
B ready

A debug_defeat_monster <monsterId>

A click_loot <dropId>
B click_loot <dropId>

A print_inventory
B print_inventory

A finish_session
A print_settlement
```

기대 결과:

- A 또는 B 중 한 명만 `LootResolved`를 받습니다.
- 승자는 `InventorySnapshot`에서 아이템과 무게가 갱신됩니다.
- 나머지 클라이언트는 `LootRejected(reason=AlreadyClaimed)`를 받습니다.
- `finish_session` 이후 `SettlementResult`가 출력됩니다.

---

## 코드 읽기 가이드

| 목적 | 시작 파일 |
| --- | --- |
| 서버 lifecycle과 TCP 처리 흐름 | `server/src/Core/Server.cpp` |
| Room / Battle / Loot / Settlement 도메인 | `server/src/Game/RoomManager.cpp` |
| TCP packet framing | `server/src/Net/TcpPacketReader.cpp`, `server/src/Net/TcpPacket.cpp` |
| Debug CLI 명령 처리 | `client/debug_cli/DebugCli.cpp`, `client/debug_cli/DebugCliCommand.cpp` |
| RUDP protocol 기반 구현 | `server/src/Net/` |
| Room Actor / WorkerPool 기반 구현 | `server/src/Game/RoomEvent.hpp`, `server/src/Game/RoomActor.cpp`, `server/src/Game/WorkerPool.cpp` |
| Meta settlement 세로 슬라이스 | `meta-server/src/main/java/com/lol/meta/settlement/` |

---

## 자세한 문서

- [아키텍처와 경계](docs/architecture.md)
- [테스트 매트릭스](docs/test_matrix.md)
- [로드맵과 현재 한계](docs/roadmap.md)

이 저장소는 포트폴리오 검토를 위한 공개용 소스 미러입니다. 내부 설계 로그, 작업 계획, 사적인 검토 문서는 포함하지 않습니다.
