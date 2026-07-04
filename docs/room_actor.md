# RoomActor / WorkerPool 처리 모델

이 문서는 README의 RoomActor / WorkerPool 설명을 보완하는 public 전용 문서입니다. 핵심은 같은 Room의 상태 변경을 한 처리 경계에서 순차 적용해 race condition을 줄이는 것입니다.

## 처리 흐름

```text
RoomEvent
  -> RoomEventQueue
  -> RoomEventDispatcher
  -> RoomActor
  -> OutboundSendQueue
  -> network send boundary
```

`RoomActor`는 Room 상태 변경의 single-writer 경계입니다. `WorkerPool`은 여러 Room으로 확장하기 위한 실행 기반이며, public README에서는 전체 운영 런타임이 완전한 multi-worker 구조로 전환됐다고 주장하지 않습니다.

## 구성

| 구성 | 책임 | 대표 위치 |
| --- | --- | --- |
| `RoomEvent` | Ready, MonsterDeath, ClickLoot 같은 내부 이벤트 표면 | `server/src/Game/RoomEvent.hpp` |
| `RoomEventQueue` | bounded FIFO, backpressure boundary, shutdown drain | `server/src/Game/RoomEventQueue.cpp` |
| `RoomEventDispatcher` | `roomId` 기준 actor routing, multi-room isolation | `server/src/Game/RoomEventDispatcher.cpp` |
| `RoomActor` | 같은 Room state mutation의 single-writer 경계 | `server/src/Game/RoomActor.cpp` |
| `OutboundSendQueue` | response/broadcast envelope를 처리 경계 밖으로 분리 | `server/src/Game/OutboundSendQueue.hpp` |
| `WorkerPool` | worker wake, shutdown lifecycle, 확장 실행 기반 | `server/src/Game/WorkerPool.cpp` |
| `RoomEventMetrics` | queue/worker baseline metric primitive | `server/src/Game/RoomEventMetrics.hpp` |

## 검증 축

| 보장하려는 동작 | 대표 테스트 |
| --- | --- |
| RoomEvent surface가 의도한 payload를 보존 | `tests/core/RoomEventTests.cpp` |
| bounded queue와 shutdown drain이 동작 | `tests/core/RoomEventQueueTests.cpp` |
| 같은 Room 이벤트가 순차 적용 | `tests/core/RoomActorTests.cpp` |
| 같은 Room의 ClickLoot 경합이 single-writer 경계에서 처리 | `tests/core/RoomActorTests.cpp` |
| 여러 Room의 상태가 서로 섞이지 않음 | `tests/core/RoomEventDispatcherTests.cpp` |
| response/broadcast가 OutboundSendQueue로 분리 | `tests/core/OutboundSendQueueTests.cpp` |
| worker wake와 shutdown lifecycle이 회귀 테스트로 고정 | `tests/core/WorkerPoolTests.cpp` |
| queue/worker metric baseline이 유지 | `tests/core/RoomEventMetricsTests.cpp`, `tests/core/WorkerPoolTests.cpp` |

## 현재 한계

- TCP gameplay path와 inline actor pump regression은 포함되어 있습니다.
- production runtime 전체가 multi-worker structure로 완전히 전환됐다는 의미는 아닙니다.
- Room ownership policy와 운영 배치 기준은 후속 확정 범위입니다.
