# Loot of Legends

C++ 게임 서버가 이동과 전투 및 루팅을 판정하는 멀티플레이 게임입니다. Spring 메타 서버는 정산을 받아 MySQL 자산에 반영합니다. 1인 개발 프로젝트이며 AI 에이전트가 구현에 참여했습니다.

[플레이 시연](https://www.youtube.com/watch?v=rjpUEDnBJJg)

## 개발 사례

| 사례 | 코드와 테스트 | 실행 기록 |
| --- | --- | --- |
| [5,000세션 수신 병목](docs/cases/receive-budget.md) | [변경 상수](docs/cases/code/receive-budget.txt) | [전후 비교](docs/cases/records/receive-comparison.json) |
| [전투 결과 만료 정리](docs/cases/memory-retention.md) | [변경 코드](docs/cases/code/memory-retention.txt) [회귀 테스트](docs/cases/code/memory-retention-test.txt) | [수정 전 RSS](docs/cases/records/rss-pre-fix-3000p.tsv) [수정 후 RSS](docs/cases/records/rss-post-fix-3000p.tsv) |
| [5,250세션 60분 실행](docs/cases/capacity-5250.md) | [판정 조건](docs/cases/records/capacity-5250.json) | [60분 RSS](docs/cases/records/rss-capacity-5250-60m.tsv) |
| [강제 종료 후 전투 복귀](docs/cases/battle-recovery.md) | [복구 코드](game-server/modules/game-flow/src/BattleContinuityRecovery.cpp) [실제 서버 테스트](tests/integration/server-entry/ServerEntryTests.cpp) | [MySQL 자산 대조](docs/cases/records/recovery-db.json) |

사례의 수치는 문서에 적힌 당시 실행 SHA의 결과입니다. 수신 병목과 메모리 정리는 과거 진단 브랜치의 실험이며 최신 제품 main에 합치지 않았습니다. 5,250세션 부하와 전투 복구도 서로 다른 버전과 환경에서 실행했습니다.

## 현재 공개 제품 코드

공개 제품 기준은 원본 main [`17c6d7fc0996e389fdf025de8a926974f9186b79`](https://github.com/FASD92/Loot-of-Legends-V2/commit/17c6d7fc0996e389fdf025de8a926974f9186b79)입니다. 게임 서버와 계약 및 관련 C++ 테스트를 이 SHA에서 동기화했습니다. 메타 서버와 Gradle 빌드 파일은 이미 같은 내용이었습니다. Unity는 C#과 asmdef 및 uGUI lock과 GUID를 맞췄습니다.

| 역할 | 코드 | 테스트 |
| --- | --- | --- |
| 서버 진입과 소켓 | [ConfiguredGameServer](game-server/app/composition-root/ConfiguredGameServer.cpp) | [서버 진입 테스트](tests/integration/server-entry/ServerEntryTests.cpp) |
| Room별 상태 변경 | [RoomExecutionCell](game-server/modules/game-flow/src/execution/RoomExecutionCell.cpp) | [Room 테스트](game-server/modules/game-flow/tests/RoomExecutionCellTests.cpp) |
| 전투 기록과 재생 | [BattleReplay](game-server/modules/battle-continuity/src/BattleReplay.cpp) | [재생 테스트](game-server/modules/battle-continuity/tests/BattleReplayTests.cpp) |
| 복구 저장소 | [ContinuityStorage](game-server/platform/battle-continuity-storage/src/ContinuityStorage.cpp) | [저장소 테스트](game-server/platform/battle-continuity-storage/tests/ContinuityStorageTests.cpp) |
| 정산 전송 | [SettlementPublisher](game-server/modules/settlement/src/application/SettlementPublisher.cpp) | [전송 테스트](tests/integration/settlement/SettlementPublisherTests.cpp) |
| MySQL 자산 반영 | [JdbcAssetStore](meta-server/src/main/java/com/fasd92/lootoflegends/meta/platform/mysql/JdbcAssetStore.java) | [정산 테스트](meta-server/src/test/java/com/fasd92/lootoflegends/meta/settlement/SettlementAcceptanceTest.java) |
| Unity 전투 복귀 | [BattleSessionReconnectClient](client/unity/Assets/LootOfLegends/Battle/BattleSessionReconnectClient.cs) | [재접속 테스트](client/unity/Assets/LootOfLegends/Tests/EditMode/BattleSessionReconnectClientTests.cs) |

[동기화 범위와 제외 항목](docs/product-source.md)

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
전투 기록 -> 복구 -> 정산 기록 -> Spring/MySQL 자산 반영
```

서로 다른 Room은 병렬로 실행합니다. RUDP 전송 확인과 게임 명령의 중복 처리는 별도 계층입니다. [구조 설명](docs/architecture.md)

## 공개본 검증

2026-09-07에 공개 저장소 checkout만 사용했습니다.

<!-- portfolio-test-count: ctest=60 load-unittest=47 -->

| 검증 | 결과 |
| --- | --- |
| CMake 구성과 전체 C++ 빌드 | PASS |
| CTest | 60/60 PASS |
| 아키텍처 검사 | 17 targets / 46 edges / 236 source files / 0 findings |
| Load 도구 단위 테스트 | 47/47 PASS |
| Meta Gradle check | BUILD SUCCESSFUL |

Unity Editor는 사용하지 않았습니다. 공개본에는 라이선스를 별도로 검토해야 하는 Presentation과 ThirdParty 미디어가 없으므로 asset 기반 테스트의 PASS를 주장하지 않습니다.

[환경과 실행 명령](docs/verification.md) [공개 범위와 한계](docs/public-evidence-limitations.md)

## 직접 검사하기

CMake 3.28 이상과 Ninja 및 C++20 컴파일러가 필요합니다. C++ 빌드에는 libcurl과 OpenSSL이 필요합니다. Meta 검증에는 Java 21과 Docker가 추가로 필요합니다.

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

공개 사례 검사는 파일 해시와 기록 관계를 확인합니다. 과거 부하나 Unity 복구를 재실행하지 않습니다.
