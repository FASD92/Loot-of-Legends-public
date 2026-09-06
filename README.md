# Loot of Legends

C++ 게임 서버가 전투와 루팅을 판정하는 멀티플레이 게임입니다. Spring 메타 서버는 정산을 받아 MySQL 자산에 반영합니다. 1인 개발 프로젝트이며 AI 에이전트가 구현에 참여했습니다.

[플레이 시연](https://www.youtube.com/watch?v=rjpUEDnBJJg)

## 개발 사례

| 문제와 검증 | 확인한 결과 | 자료 |
| --- | --- | --- |
| 5,000세션 수신 병목 | 수신 상한 변경 후 실패 Room 240개에서 0개 | [변경 코드와 전후 기록](docs/cases/receive-budget.md) |
| 반복 전투의 메모리 누적 | 결과 삽입 전에 만료 정리를 호출 | [정리 코드와 회귀 테스트](docs/cases/memory-retention.md) |
| 5,250세션 60분 실행 | 525개 Room 완주와 실패 0개 | [실행 조건과 판정 기록](docs/cases/capacity-5250.md) |
| 프로세스 강제 종료 | 클라이언트 2개가 전투에 복귀한 뒤 자산 대조 | [복구 관측과 DB 조회](docs/cases/battle-recovery.md) |

각 수치는 문서에 표시한 당시 실행 버전의 결과입니다. 5,250세션 부하와 전투 복구는 서로 다른 버전과 환경에서 확인했습니다.

**공개 상태:** 후속 사례의 선별 기록과 코드 발췌를 추가했습니다. 루트의 실행 코드는 2026-08-13 공개 기준을 유지합니다. 최신 제품 코드 전체의 공개 동기화는 아직 완료하지 않았습니다. [공개 파일 목록과 범위](docs/cases/README.md)를 함께 확인해 주세요.

## 서버 구조

```text
Unity Client
    |
TCP/UDP 수신
    |
RoomCommandGateway
    |
RoomExecutionCell ---- 같은 Room의 상태는 한 worker만 변경
    |
BattleInstance ------- 이동과 전투 및 루팅 판정
    |
정산 기록 -> publisher -> Spring/MySQL inbox -> 자산 반영
```

서로 다른 Room은 병렬로 실행합니다. RUDP 전송 확인과 게임 명령의 중복 처리 방지는 별도로 다룹니다. [구조 설명](docs/architecture.md)

## 실행 가능한 공개 코드

| 역할 | 코드 | 테스트 |
| --- | --- | --- |
| TCP/UDP 런타임 | [ConfiguredGameServer](game-server/app/composition-root/ConfiguredGameServer.cpp) | [서버 진입 경로](tests/integration/server-entry/ServerEntryTests.cpp) |
| Room별 상태 변경 | [RoomExecutionCell](game-server/modules/game-flow/src/execution/RoomExecutionCell.cpp) | [Room 테스트](game-server/modules/game-flow/tests/RoomExecutionCellTests.cpp) |
| RUDP 재전송 | [ReliableQueue](game-server/platform/transport-rudp/src/ReliableQueue.cpp) | [전송 테스트](tests/transport-rudp/RudpDeliveryTests.cpp) |
| 전투와 루팅 | [BattleInstance](game-server/modules/battle/src/domain/BattleInstance.cpp) | [루팅 테스트](game-server/modules/battle/tests/LootClaimTests.cpp) |
| 정산 수락 | [JdbcSettlementInbox](meta-server/src/main/java/com/fasd92/lootoflegends/meta/platform/mysql/JdbcSettlementInbox.java) | [정산 테스트](meta-server/src/test/java/com/fasd92/lootoflegends/meta/settlement/SettlementAcceptanceTest.java) |

## 기존 공개 코드의 검증

아래는 2026-08-13에 기록한 결과입니다. 후속 사례의 실험이나 이번 공개 업데이트에서 새로 실행한 결과와 구분합니다.

<!-- portfolio-test-count: ctest=46 load-unittest=47 -->

| 검증 | 당시 결과 |
| --- | --- |
| C++ CTest | 46/46 PASS |
| Load 도구 단위 테스트 | 47/47 PASS |
| Linux 공개 CI | PASS |
| Meta Gradle check | BUILD SUCCESSFUL |
| 실제 서버를 사용한 10-player fixture | COMPLETE / NOT_CLASSIFIED |

[검증 환경과 명령](docs/verification.md)에서 원래 기록을 확인할 수 있습니다. 공개 루트의 동시접속 수용량은 별도로 입증하지 않았습니다. [후속 5,250세션 실행](docs/cases/capacity-5250.md)은 당시 진단본의 결과입니다.

## 직접 검사하기

CMake 3.28 이상과 Ninja 및 C++20 컴파일러가 필요합니다. Python은 3.12 이상을 사용합니다. C++ 빌드에는 libcurl과 OpenSSL이 필요합니다. Meta 검증에는 Java 21과 Docker가 추가로 필요합니다.

```bash
python3 -m unittest scripts/public/test_verify_portfolio_claims.py -v
python3 -m unittest scripts/public/test_case_records.py -v
python3 scripts/public/verify_portfolio_claims.py

cmake --preset dev-debug
cmake --build --preset dev-debug --parallel 4
ctest --preset dev-debug --output-on-failure

python3 -m unittest discover -s tools/load/tests -p 'test_*.py' -v

cd meta-server
./gradlew --no-daemon --dependency-verification strict clean check
```

공개 사례 검사는 파일 해시와 기록의 일관성을 확인합니다. 과거 부하나 Unity 복구를 재실행하지 않습니다. [공개 범위와 남은 검증](docs/public-evidence-limitations.md)
