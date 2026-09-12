---
doc_id: ADR-0019
title: 중요 사건 기준 전투 저장
status: Accepted
version: 1.0.0
last_updated: 2026-09-12
authoritative_for:
  - battle-critical-event-persistence
depends_on:
  - ADR-0017
supersedes: []
---

# 중요 사건 기준 전투 저장

## 승인과 범위

사용자는 중요 사건에만 저장하고 마지막 중요 저장 이후의 모든 변경이 되돌아가도
괜찮다고 승인했다. 2026-09-12 별도 대화에서 기둥 브랜치 전체 병합 대신 최신 main에
저장 정책만 분리·이식하여 검증·커밋·PR·main 병합하는 것을 승인했다.

이 결정은 ADR-0017의 Decision 5 저장 빈도/일반 출력 RPO와 기존 주기 checkpoint 조항만
대체한다. 기존 계약 1.0.4와 Slice 13 evidence는 최초 활성화 당시 기록으로 그대로
보존한다. 현재 runtime에는 기계 계약
`contracts/battle-continuity/critical-event-policy-v1.json@1.0.0`의 명시된 override가
우선한다. 그 외 identity, codec, 복구 검증, 개인정보, settlement 순서는 기존 계약이다.

## 결정

- BattleRecording이 기존 canonical command/TickCommit를 RAM에 보존한다.
- 일반 Move/MovementTick/비치명적 Attack은 저장 요청 없이 결과를 내보낸다.
- Battle 시작, load/input/membership 변경, phase 전이, monster 처치, drop/holdings
  변경, terminal에서는 누적 기록과 전체 checkpoint를 함께 append하고 sync한다.
- 같은 상태의 중복·거절은 새 기록/저장을 만들지 않는다. 상태를 바꾸는 거절은 기존
  admission 규칙을 유지하며 실제 변경 종류에 따라 기록한다.
- 주기 보조 저장은 없다. 일반 변경은 시간 상한 없이 전부 rollback될 수 있다.
- 중요 결과는 해당 write 완료 전 내보내지 않는다. 기존 Cell의 한 pending write와
  completion identity/epoch/sequence 검사를 재사용한다.
- public RecordedTickBatch의 first/last sequence는 여러 완결 tick을 포함할 수 있고
  logicalTick은 마지막 tick이다. 각 tick의 CRC/sequence/hash 경계 검증은 유지한다.
- 누적 encoded history 한도는 기존 64MiB이며 초과는 fail closed다. C++ 객체 overhead를
  포함한 정확한 RSS 제한을 뜻하지 않는다.
- 저장 worker의 검증된 journal tail cache는 최대 1,024개다. 파일 변경/실패/재시작은
  cold validation하고, 모든 제출 요청에서 실제 sync를 유지한다.

## 비목표와 호환성

기둥 gameplay, HP 도입, 새 wire/state/journal schema, Unity, 새 queue/owner/dependency,
DB migration, AWS/deploy, Product Release/Capacity claim은 포함하지 않는다.
기존 main journal은 그대로 복구한다. 기둥 브랜치의 V2 journal은 이 runtime 대상이 아니다.
원래 기둥 작업 트리와 미커밋 변경은 수정하지 않는다.

## 검증

ordinary tick 무저장, critical 누적 flush, crash prefix rollback, replay 최종 hash,
multi-tick storage 검증/실패, Cell output gate와 기존 C++/문서 회귀를 실행한다.
main에 분리한 source의 증거를 새로 만들며 이전 기둥/Windows 결과를 빌려 쓰지 않는다.
actual Unity 강제종료 실증과 새 capacity claim은 별도 미검증으로 표시한다.
