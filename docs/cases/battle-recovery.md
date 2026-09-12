# 중요 사건 저장과 전투 복구

게임 서버는 일반 이동과 비치명적 공격을 RAM에 기록하고 즉시 결과를 보냅니다. 전투 시작과 상태 전환, 몬스터 처치, 전리품 변경, 전투 종료 같은 중요 사건에서는 누적 기록과 전체 checkpoint를 디스크에 동기화한 뒤 중요 결과를 보냅니다.

주기 저장은 없습니다. 프로세스 장애가 나면 마지막 중요 저장 이후의 일반 변경은 모두 되돌아갈 수 있습니다. 저장을 마친 중요 결과는 동일 호스트와 디스크가 유지되는 조건에서 보존합니다.

구현은 [저장 판정](../../game-server/modules/battle-continuity/src/FlightRecorder.cpp), [저장소](../../game-server/platform/battle-continuity-storage/src/ContinuityStorage.cpp), [정책 계약](../../contracts/battle-continuity/critical-event-policy-v1.json)에서 확인할 수 있습니다. 정책 결정은 [ADR-0019](../adr/ADR-0019-critical-event-battle-persistence.md)에 기록했습니다.

## 현재 정책의 서버 진입 검사

실제 GameServer child process와 TCP/UDP 테스트 클라이언트로 다음 경로를 검사했습니다.

- 일반 이동과 공격 뒤 SIGKILL 시 마지막 중요 저장 지점의 위치와 HP로 복귀
- 같은 Session과 Room 및 Battle 유지, 새 generation 적용, stale transport 차단
- 전체 snapshot 적용 ACK 뒤 입력 재개
- 복귀 뒤 공격과 loot 및 Result 완료
- terminal 재시작 뒤 정산 기록 중복 없음

[서버 진입 테스트](../../tests/integration/server-entry/ServerEntryTests.cpp)는 이 경로를 실행합니다. 새 정책의 실제 Unity 강제 종료 실행과 외부 MySQL 대조는 수행하지 않았습니다.

## Unity와 MySQL 실행 기록

2026-09-02에는 매 logical tick을 동기화하던 정책으로 실제 GameServer 프로세스에 SIGKILL을 보낸 뒤 같은 실행 파일과 endpoint로 다시 시작했습니다. Unity Standalone 두 개가 기존 전투로 돌아와 이동과 공격을 이어갔습니다. 루팅과 Result 및 Collection까지 완료한 뒤 MySQL을 조회했습니다.

| 항목 | 결과 |
| --- | --- |
| 프로세스 종료 | SIGKILL 후 exit -9 |
| 전투 복원 | Battle 1개 |
| 플레이어 복귀 | Unity 클라이언트 2개 |
| 식별자 | Session과 Room 및 Battle 유지 |
| 입력 재개 | snapshot 적용 ACK 이후 |
| 재생 | GCC와 Clang의 동일 journal 최종 상태 해시 일치 |

실행 source는 `ee4654daf5611b93b7a80b62d16d3f06018da97e`입니다.

## 재시작 후 DB 대조

| 항목 | Collection 완료 후 | epoch 3 재시작 후 | epoch 4 재시작 후 |
| --- | ---: | ---: | ---: |
| 적용된 정산 | 2 | 2 | 2 |
| 아이템 수량 합계 | 2 | 2 | 2 |
| 지갑 잔액 합계 | 400 | 400 | 400 |

[DB 조회 기록](records/recovery-db.json)은 네 조회 시점의 집계값을 담습니다.

정산은 Game의 [SettlementPublisher](../../game-server/modules/settlement/src/application/SettlementPublisher.cpp)가 Meta에 보내고, Meta의 [SettlementApplication](../../meta-server/src/main/java/com/fasd92/lootoflegends/meta/settlement/application/SettlementApplication.java)이 동일 ID와 payload hash를 확인합니다. [SettlementApplyApplication](../../meta-server/src/main/java/com/fasd92/lootoflegends/meta/settlement/application/SettlementApplyApplication.java)은 한 transaction에서 자산을 적용한 뒤 inbox를 Applied로 바꿉니다. [MySQL 정산 테스트](../../meta-server/src/test/java/com/fasd92/lootoflegends/meta/settlement/SettlementAcceptanceTest.java)는 replay와 conflict 및 rollback을 검사합니다.

## 범위

복구는 같은 호스트와 endpoint 및 local durable filesystem을 전제로 합니다. 다중 호스트 전환과 디스크 손실 및 대규모 부하 중 복구는 검증하지 않았습니다. 현재 정책의 결과 손실 0은 저장 완료 후 전송한 중요 결과에만 적용합니다.

[사례 목록](README.md)
