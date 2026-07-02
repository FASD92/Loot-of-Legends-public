# 아키텍처

이 문서는 README의 구조도를 보완하는 public 전용 설명입니다. 내부 설계 로그가 아니라, 현재 공개 소스가 어떤 runtime 경계와 검증 경계를 갖는지 요약합니다.

## 기준 구조

```text
Unity Player Client
  -> TCP gameplay command / RUDP input
  -> C++ Core::Server
  -> Session / Room / RUDP peer state
  -> RoomManager / RoomActor foundation
  -> Loot / Inventory / SettlementResult
  -> TCP response, RUDP event, StateSnapshot

Spring Meta Server
  -> OAuth / account
  -> admission queue
  -> game session token
  -> settlement API
  -> MySQL / Redis
```

핵심 기준은 단순합니다. 클라이언트는 intent를 보내고, 서버가 상태를 확정합니다. 이동, 공격, 루팅, 정산 모두 이 경계를 흐리지 않도록 구성했습니다.

## C++ Game Server

| 모듈 | 책임 |
| --- | --- |
| `Core::Server` | TCP listener, UDP/RUDP receive path, session lifecycle, room command routing |
| `SessionManager` | session id 발급과 연결 생명주기 |
| `RoomManager` / `Room` | Room membership, Ready, BattleStart, MonsterDeath, Drop, Loot, Inventory, SettlementResult |
| `RoomActor` / `WorkerPool` | RoomEvent single-writer foundation과 병렬 처리 기반 |
| `OutboundSendQueue` | response/broadcast envelope를 Room 처리와 전송 경계에서 분리 |
| `PrometheusTextfileWriter` | capacity/stability 측정용 metrics textfile 출력 |

현재 production path에서 모든 Room 처리가 완전한 multi-worker runtime으로 전환됐다는 뜻은 아닙니다. public 기준으로는 TCP gameplay path와 inline actor pump regression, WorkerPool foundation을 함께 보여줍니다.

## Meta Server

Spring Meta Server는 게임 루프의 hot path가 아니라 account/session/settlement boundary입니다.

| 영역 | 책임 |
| --- | --- |
| Auth | OAuth account resolution, principal normalization |
| Admission | active session capacity, queue, reservation, CSRF boundary |
| Session token | C++ server가 game-session authentication에 사용할 claim/renew/release contract |
| Settlement | settlement idempotency, request hash, rollback invariant, processing guard |
| Storage | MySQL schema, Redis processing/session guard |

이 경계는 C++ gameplay hot path에 DB transaction을 섞지 않기 위한 분리입니다.

## Custom RUDP

RUDP는 “UDP socket을 열었다”가 아니라 다음 레이어로 나눠 구현했습니다.

| 레이어 | 설명 |
| --- | --- |
| Packet codec | packet type, sequence, ack, payload encode/decode |
| Hello binding | UDP endpoint를 TCP-authenticated session과 연결 |
| Input command | movement/attack/loot intent payload와 `cmdSeq` gate |
| ACK/retransmission | pending packet metadata, deadline scan, resend flush |
| Reliable ordered event | server-origin event id, pending ACK, duplicate delivery guard |
| Room dispatch | validated input을 RoomEvent 경계로 변환 |
| StateSnapshot | 서버 권위 위치 상태를 client render surface로 전달 |

현재 README에서 RUDP를 강조하는 이유는 프로토콜을 직접 구성한 흔적을 보여주기 위해서입니다. 다만 전체 gameplay transport가 RUDP로 완전히 이전됐다는 주장은 하지 않습니다.

## 서버 권한 루팅 흐름

```text
MonsterDeath
  -> server creates Drop
  -> clients receive DropListSnapshot
  -> multiple clients send ClickLoot
  -> server checks Room membership, drop state, inventory weight
  -> one client receives LootResolved
  -> others receive LootRejected
  -> winner receives InventorySnapshot
```

중요한 점은 `ClickLoot` 순서가 client 주장으로 결정되지 않는다는 것입니다. 서버가 Drop 상태와 Inventory 상태를 기준으로 결과를 확정합니다.

## Capacity / Stability 경계

capacity 관련 코드는 결과 숫자를 크게 보이게 하려는 목적보다, “무엇을 PASS로 볼 것인가”를 분리하기 위한 목적이 큽니다.

| 경계 | 의미 |
| --- | --- |
| 100-session handoff | active session capacity와 queue promotion, game-session authentication handoff 검증 |
| Release1 capacity report | latency, delivery, tick/stability, resource gate를 판정하는 artifact contract |
| 670 local diagnostic | safe capacity claim이 아니라 failure stage, backlog, errno/latency 해석을 위한 로컬 진단 |
| Prometheus textfile | runtime metric을 report script가 읽을 수 있는 형태로 출력 |

public repo에는 raw private experiment log를 넣지 않습니다. 대신 source, test, schema, report evaluator만 공개합니다.
