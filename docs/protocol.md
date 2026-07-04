# Custom RUDP 프로토콜

이 문서는 README의 Custom RUDP 설명을 보완하는 public 전용 문서입니다. 목적은 “UDP를 썼다”가 아니라, UDP 위에 어떤 신뢰성 계층을 직접 구성했는지 코드와 테스트 기준으로 보여주는 것입니다.

## 현재 범위

| 레이어 | 책임 | 대표 위치 |
| --- | --- | --- |
| Packet codec | packet type, sequence, ACK, payload encode/decode | `server/src/Net/RudpPacket.cpp`, `server/src/Net/RudpGameEventPayload.cpp` |
| Session binding | UDP endpoint를 TCP-authenticated session과 연결 | `server/src/Net/RudpSessionBinder.cpp` |
| Input command gate | `cmdSeq` 기반 이전/중복 입력 거절 | `server/src/Net/RudpInputCommandSequenceTracker.cpp`, `server/src/Net/RudpMoveInputGuard.cpp` |
| ACK / retransmission | pending metadata, deadline scan, resend flush | `server/src/Net/RudpReliableSendQueue.cpp`, `server/src/Net/RudpRetransmissionScan.cpp` |
| Reliable ordered event | server-origin event id, pending ACK, ordered delivery | `server/src/Net/RudpReliableEventSendQueue.cpp` |
| Duplicate guard | 같은 server event의 중복 전달 방지 | `server/src/Net/RudpGameplayEventIdempotencyTracker.cpp`, `server/src/Net/RudpMetaResponseIdempotencyTracker.cpp` |
| RoomEvent translation | 검증된 RUDP input을 Room 처리 경계로 변환 | `server/src/Core/RudpInputCommandRoomEventTranslator.cpp` |

## 검증 축

| 보장하려는 동작 | 대표 테스트 |
| --- | --- |
| packet parser / serializer가 payload boundary를 유지 | `tests/protocol/PacketParserTests.cpp`, `tests/protocol/PacketSerializerTests.cpp` |
| ACK window와 pending queue가 재전송 기준을 유지 | `tests/protocol/AckWindowTests.cpp`, `tests/protocol/ReliableSendQueueTests.cpp` |
| retransmission scan/flush가 deadline 기준으로 동작 | `tests/protocol/RudpRetransmissionScanTests.cpp`, `tests/protocol/RudpRetransmissionFlushTests.cpp` |
| Hello payload와 UDP endpoint가 session에 bind | `tests/protocol/RudpHelloPayloadTests.cpp`, `tests/protocol/RudpSessionBinderTests.cpp` |
| 오래된 input과 중복 input이 reject | `tests/protocol/RudpInputCommandSequenceTrackerTests.cpp`, `tests/protocol/RudpMoveInputGuardTests.cpp` |
| server event가 ACK 전까지 pending 상태로 남고 순서대로 전송 | `tests/protocol/RudpReliableEventSendQueueTests.cpp` |
| duplicate event delivery가 Room state를 다시 변경하지 않음 | `tests/protocol/RudpReliableEventDeliveryGuardTests.cpp`, `tests/protocol/RudpGameplayEventIdempotencyTrackerTests.cpp` |
| RUDP input이 RoomEvent 경계로 변환 | `tests/core/RudpInputCommandRoomEventTranslatorTests.cpp` |

## 현재 한계

- README의 기준 gameplay path는 TCP입니다.
- RUDP는 protocol/input/reliable server event 계층 중심으로 구현/검증했습니다.
- 모든 gameplay 결과 전송이 RUDP로 완전히 이전됐다고 주장하지 않습니다.
- loss/jitter/reorder 환경의 장시간 soak evidence는 후속 보강 범위입니다.
