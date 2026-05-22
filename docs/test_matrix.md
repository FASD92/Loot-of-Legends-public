# 테스트 매트릭스

이 문서는 public repo에서 어떤 불변식을 어떤 테스트가 보호하는지 빠르게 확인하기 위한 공개용 요약입니다.

## 핵심 gameplay

| 불변식 / 동작 | 대표 테스트 |
| --- | --- |
| 한 세션은 동시에 하나의 Room에만 속함 | `tests/core/RoomManagerTests.cpp` |
| 2인 Room에 세 번째 플레이어가 들어오면 `Full`로 거절 | `tests/core/RoomManagerTests.cpp` |
| 두 플레이어가 모두 Ready일 때만 `BattleStart` 발생 | `tests/core/RoomManagerTests.cpp`, `tests/core/ServerRoomIntegrationTests.cpp` |
| 중복 Ready가 BattleStart를 중복 발생시키지 않음 | `tests/core/RoomManagerTests.cpp` |
| MonsterDeath 이후 DropListSnapshot 전파 | `tests/core/RoomManagerTests.cpp`, `tests/core/ServerRoomIntegrationTests.cpp` |
| 같은 Drop은 한 명만 claim 가능 | `tests/core/RoomManagerTests.cpp`, `tests/core/ServerRoomIntegrationTests.cpp` |
| 이미 claim된 Drop은 owner를 바꾸지 않음 | `tests/core/RoomManagerTests.cpp` |
| 무게 초과 시 Drop과 Inventory가 변경되지 않음 | `tests/core/RoomManagerTests.cpp` |
| 반복 `finish_session` 요청은 같은 payload 반환 | `tests/core/RoomManagerTests.cpp`, `tests/core/ServerRoomIntegrationTests.cpp` |

## TCP / CLI

| 영역 | 대표 테스트 |
| --- | --- |
| TCP packet serialization | `tests/core/TcpPacketTests.cpp` |
| TCP stream framing / partial read | `tests/core/TcpPacketReaderTests.cpp` |
| TCP listener lifecycle | `tests/core/TcpListenerTests.cpp` |
| session id 발급과 제거 | `tests/core/SessionManagerTests.cpp` |
| 실제 TCP 서버 통합 흐름 | `tests/core/ServerIntegrationTests.cpp`, `tests/core/ServerRoomIntegrationTests.cpp` |
| Debug CLI command parsing | `tests/client/DebugCliCommandTests.cpp` |

## RUDP protocol / event layer

| 영역 | 대표 테스트 |
| --- | --- |
| packet parser / serializer | `tests/protocol/PacketParserTests.cpp`, `tests/protocol/PacketSerializerTests.cpp` |
| ACK window | `tests/protocol/AckWindowTests.cpp` |
| reliable send queue | `tests/protocol/ReliableSendQueueTests.cpp` |
| retransmission scan / flush | `tests/protocol/RudpRetransmissionScanTests.cpp`, `tests/protocol/RudpRetransmissionFlushTests.cpp` |
| receive pipeline / socket drain | `tests/protocol/RudpReceivePipelineTests.cpp`, `tests/protocol/RudpSocketDrainTests.cpp` |
| Hello session binding | `tests/protocol/RudpHelloPayloadTests.cpp`, `tests/protocol/RudpSessionBinderTests.cpp` |
| InputCommand payload와 `cmdSeq` gate | `tests/protocol/RudpInputCommandPayloadTests.cpp`, `tests/protocol/RudpInputCommandSequenceTrackerTests.cpp` |
| Reliable Ordered event payload | `tests/protocol/RudpBattleStartPayloadTests.cpp`, `tests/protocol/RudpGameEventPayloadTests.cpp`, `tests/protocol/RudpMetaResponsePayloadTests.cpp` |
| event idempotency / duplicate guard | `tests/protocol/RudpGameplayEventIdempotencyTrackerTests.cpp`, `tests/protocol/RudpMetaResponseIdempotencyTrackerTests.cpp`, `tests/protocol/RudpReliableEventDeliveryGuardTests.cpp` |
| event pending queue | `tests/protocol/RudpReliableEventSendQueueTests.cpp` |

## Room Actor / WorkerPool

| 영역 | 대표 테스트 |
| --- | --- |
| internal event surface | `tests/core/RoomEventTests.cpp` |
| bounded FIFO / backpressure | `tests/core/RoomEventQueueTests.cpp` |
| actor apply와 single-writer 경계 | `tests/core/RoomActorTests.cpp` |
| same-room ClickLoot 경합 | `tests/core/RoomActorTests.cpp` |
| multi-room state isolation | `tests/core/RoomActorTests.cpp`, `tests/core/RoomEventDispatcherTests.cpp` |
| outbound response/broadcast envelope | `tests/core/OutboundSendQueueTests.cpp` |
| worker wake / shutdown lifecycle | `tests/core/WorkerPoolTests.cpp` |
| shutdown drain과 duplicate 방지 | `tests/core/WorkerPoolTests.cpp` |
| queue / worker metric baseline | `tests/core/RoomEventMetricsTests.cpp`, `tests/core/WorkerPoolTests.cpp` |
| TCP inline actor pump regression | `tests/core/ServerRoomIntegrationTests.cpp`, `tests/core/ServerIntegrationTests.cpp` |

## Meta settlement

| 영역 | 대표 테스트 |
| --- | --- |
| API contract | `meta-server/src/test/java/com/lol/meta/SettlementApiContractTests.java` |
| schema migration | `meta-server/src/test/java/com/lol/meta/SettlementSchemaMigrationTests.java` |
| repository behavior | `meta-server/src/test/java/com/lol/meta/SettlementRepositoryTests.java` |
| service idempotency | `meta-server/src/test/java/com/lol/meta/settlement/SettlementServiceIdempotencyTests.java` |
| rollback invariant | `meta-server/src/test/java/com/lol/meta/settlement/SettlementRollbackInvariantTests.java` |
| processing guard | `meta-server/src/test/java/com/lol/meta/settlement/SettlementProcessingGuardTests.java` |
| internal credential interceptor | `meta-server/src/test/java/com/lol/meta/internal/InternalCredentialInterceptorTests.java` |
| vertical slice | `meta-server/src/test/java/com/lol/meta/settlement/SettlementApiVerticalSliceTests.java` |

## 실행 명령

C++ 테스트:

```bash
ctest --test-dir build --output-on-failure
```

Meta Server 테스트:

```bash
cd meta-server
./gradlew test
```
