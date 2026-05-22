# 아키텍처와 경계

이 문서는 public README에서 줄인 구조 설명을 보완하기 위한 공개용 요약입니다. 내부 작업 로그가 아니라, 현재 공개 소스가 어떤 경계로 동작하고 어디까지 검증됐는지 설명합니다.

## 기준 경로

현재 검증 중심은 C++ TCP gameplay path입니다.

```text
Debug CLI / TCP client
  -> Core::Server
  -> TcpPacketReader
  -> SessionManager
  -> RoomManager
  -> Room / Drop / Inventory / SettlementResult
  -> TCP response / broadcast
```

클라이언트는 `ready`, `debug_defeat_monster`, `click_loot`, `finish_session` 같은 요청을 보냅니다. 서버는 Room 상태, Drop 상태, Inventory 무게, SettlementResult cache를 기준으로 최종 결과를 결정합니다.

## 서버 권한 gameplay 흐름

```text
Connect
  -> Welcome(sessionId)
  -> CreateRoom / JoinRoom
  -> Ready
  -> BattleStart
  -> MonsterSpawn
  -> MonsterDeath
  -> DropListSnapshot
  -> ClickLoot race
  -> LootResolved or LootRejected
  -> InventorySnapshot
  -> FinishSessionRequest
  -> SettlementResult
```

핵심은 같은 Drop에 대한 경쟁 요청입니다. 여러 클라이언트가 같은 Drop을 클릭해도 서버는 한 명에게만 소유권을 확정하고, 나머지 요청은 기존 소유자를 바꾸지 않고 거절합니다.

## Meta settlement 경계

C++ 서버는 `SettlementResult` payload를 생성하고, 같은 세션의 반복 `finish_session` 요청에 동일 payload를 반환합니다. 이 멱등성은 C++ 서버 프로세스 메모리 기준입니다.

Spring Meta Server는 별도 모듈로 다음을 검증합니다.

- internal token 검증
- settlement request validation
- MySQL `settlementId` 기반 중복 방어
- transaction rollback invariant
- Redis 기반 processing guard

아직 C++ 서버가 Spring Meta Server를 runtime HTTP로 호출하지는 않습니다. 현재 public repo에서 두 영역은 계약과 테스트 세로 슬라이스로 분리되어 있습니다.

## RUDP 경계

RUDP는 TCP gameplay path를 즉시 대체하지 않습니다. 현재 구현은 전송 계층과 event layer 기반을 검증하는 단계입니다.

현재 포함된 범위:

- packet parser / serializer
- ACK window
- reliable send queue
- retransmission scan / flush
- UDP socket drain
- Hello session binding
- InputCommand payload와 `cmdSeq` gate
- Reliable Ordered event payload / queue / duplicate guard

후속 범위:

- 실제 UDP datagram 송신 경로 연결
- server-loop ACK consume과 event pending queue의 runtime 연결
- RUDP `InputCommand -> RoomEvent` gameplay dispatch
- outbound transport policy 확정

## Room Actor / WorkerPool 경계

Room Actor / WorkerPool 영역은 같은 Room을 한 writer가 순차 처리하고, 서로 다른 Room은 병렬 처리할 수 있게 만들기 위한 foundation입니다.

현재 포함된 범위:

- `RoomEvent`
- bounded `RoomEventQueue`
- `RoomActor`
- `RoomEventDispatcher`
- `OutboundSendQueue`
- `WorkerPool`
- `RoomEventMetrics`
- TCP `Ready`, `MonsterDeath`, `ClickLoot` inline actor pump regression

현재 production `Core::Server`는 TCP `Ready`, `MonsterDeath`, `ClickLoot`를 inline actor pump로 처리합니다. production WorkerPool thread를 `Core::Server` runtime에 연결하는 것은 후속 범위입니다.

## 변경하지 않은 public 계약

다음 계약은 확장 작업 중에도 유지합니다.

- TCP packet schema
- Debug CLI 기준 시연 흐름
- `CreateRoom`, `JoinRoom`, `LeaveRoom`의 기존 RoomManager 경로
- `FinishSessionRequest` / `SettlementResult` 계약
- C++ gameplay hot path와 DB write path의 분리
