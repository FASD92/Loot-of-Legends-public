# 테스트 매트릭스

이 문서는 README에서 언급한 검증 근거를 파일 단위로 추적하기 위한 public 문서입니다.

## 테스트 선언 집계

현재 공개 소스 기준 단순 테스트 선언 집계:

| 영역 | 기준 | 개수 |
| --- | --- | ---: |
| C++ GoogleTest | `TEST`, `TEST_F` | 659 |
| Meta Server JUnit | `@Test`, `@ParameterizedTest` | 221 |
| Unity EditMode | `[Test]` | 388 |
| 합계 | 단순 테스트 선언 합산 | 1,268 |

이 숫자는 “모든 테스트가 방금 실행됐다”는 뜻이 아닙니다. 공개 소스 안에 존재하는 회귀 테스트 선언 규모를 보여주는 지표입니다.

## Gameplay Invariants

| 불변식 / 동작 | 대표 테스트 |
| --- | --- |
| 한 세션은 동시에 하나의 Room에만 속함 | `tests/core/RoomManagerTests.cpp` |
| 룸 정원 초과 join은 거절 | `tests/core/RoomManagerTests.cpp` |
| 모든 현재 멤버 Ready 이후 `BattleStart` 발생 | `tests/core/RoomManagerTests.cpp`, `tests/core/ServerRoomIntegrationTests.cpp` |
| 중복 Ready가 BattleStart를 중복 발생시키지 않음 | `tests/core/RoomManagerTests.cpp` |
| MonsterDeath 이후 DropListSnapshot 전파 | `tests/core/RoomManagerTests.cpp`, `tests/core/ServerRoomIntegrationTests.cpp` |
| 같은 Drop은 한 명만 claim 가능 | `tests/core/RoomManagerTests.cpp`, `tests/core/ServerRoomIntegrationTests.cpp` |
| 이미 claim된 Drop은 owner를 바꾸지 않음 | `tests/core/RoomManagerTests.cpp` |
| 무게 초과 시 Drop과 Inventory가 변경되지 않음 | `tests/core/RoomManagerTests.cpp` |
| 반복 finish session은 같은 payload 반환 | `tests/core/RoomManagerTests.cpp`, `tests/core/ServerRoomIntegrationTests.cpp` |

## Network / Runtime

| 영역 | 대표 테스트 |
| --- | --- |
| TCP packet serialization | `tests/core/TcpPacketTests.cpp` |
| TCP stream framing / partial read | `tests/core/TcpPacketReaderTests.cpp` |
| TCP listener lifecycle | `tests/core/TcpListenerTests.cpp` |
| session id 발급과 제거 | `tests/core/SessionManagerTests.cpp` |
| 실제 TCP 서버 통합 흐름 | `tests/core/ServerIntegrationTests.cpp`, `tests/core/ServerRoomIntegrationTests.cpp` |
| UDP socket edge | `tests/core/UdpSocketTests.cpp` |
| `NetworkEventLoop` contract | `tests/core/NetworkEventLoopTests.cpp` |
| Linux epoll readiness | `tests/core/EpollEventLoopTests.cpp` |
| epoll runtime stress target | `tests/stress/EpollRuntimeStress.cpp` |

## Custom RUDP

| 영역 | 대표 테스트 |
| --- | --- |
| packet parser / serializer | `tests/protocol/PacketParserTests.cpp`, `tests/protocol/PacketSerializerTests.cpp` |
| ACK window | `tests/protocol/AckWindowTests.cpp` |
| reliable send queue | `tests/protocol/ReliableSendQueueTests.cpp` |
| retransmission scan / flush | `tests/protocol/RudpRetransmissionScanTests.cpp`, `tests/protocol/RudpRetransmissionFlushTests.cpp` |
| receive pipeline / socket drain | `tests/protocol/RudpReceivePipelineTests.cpp`, `tests/protocol/RudpSocketDrainTests.cpp` |
| Hello session binding | `tests/protocol/RudpHelloPayloadTests.cpp`, `tests/protocol/RudpSessionBinderTests.cpp` |
| InputCommand payload와 `cmdSeq` gate | `tests/protocol/RudpInputCommandPayloadTests.cpp`, `tests/protocol/RudpInputCommandSequenceTrackerTests.cpp` |
| Move InputCommand guard | `tests/protocol/RudpMoveInputGuardTests.cpp` |
| RUDP `InputCommand -> RoomEvent` translation | `tests/core/RudpInputCommandRoomEventTranslatorTests.cpp` |
| StateSnapshot payload | `tests/protocol/RudpStateSnapshotPayloadTests.cpp` |
| Reliable Ordered event payload | `tests/protocol/RudpBattleStartPayloadTests.cpp`, `tests/protocol/RudpBattleStartRosterPayloadTests.cpp`, `tests/protocol/RudpGameEventPayloadTests.cpp`, `tests/protocol/RudpMetaResponsePayloadTests.cpp` |
| event idempotency / duplicate guard | `tests/protocol/RudpGameplayEventIdempotencyTrackerTests.cpp`, `tests/protocol/RudpMetaResponseIdempotencyTrackerTests.cpp`, `tests/protocol/RudpReliableEventDeliveryGuardTests.cpp` |
| reliable event pending queue | `tests/protocol/RudpReliableEventSendQueueTests.cpp` |

## Room Actor / WorkerPool

| 영역 | 대표 테스트 |
| --- | --- |
| internal event surface | `tests/core/RoomEventTests.cpp` |
| bounded FIFO / backpressure | `tests/core/RoomEventQueueTests.cpp` |
| actor apply와 single-writer 경계 | `tests/core/RoomActorTests.cpp` |
| same-room ClickLoot 경합 | `tests/core/RoomActorTests.cpp` |
| multi-room state isolation | `tests/core/RoomEventDispatcherTests.cpp` |
| outbound response/broadcast envelope | `tests/core/OutboundSendQueueTests.cpp` |
| worker wake / shutdown lifecycle | `tests/core/WorkerPoolTests.cpp` |
| queue / worker metric baseline | `tests/core/RoomEventMetricsTests.cpp`, `tests/core/WorkerPoolTests.cpp` |
| TCP inline actor pump regression | `tests/core/ServerRoomIntegrationTests.cpp`, `tests/core/ServerIntegrationTests.cpp` |

## Meta Server

| 영역 | 대표 테스트 |
| --- | --- |
| OAuth/security config | `meta-server/src/test/java/com/lol/meta/auth/Release0SecurityConfigOAuthTests.java`, `meta-server/src/test/java/com/lol/meta/auth/AuthControllerTests.java` |
| player account / nickname | `meta-server/src/test/java/com/lol/meta/auth/PlayerAccountRepositoryTests.java`, `meta-server/src/test/java/com/lol/meta/auth/PlayerNicknameTests.java` |
| admission / queue | `meta-server/src/test/java/com/lol/meta/admission/AdmissionServiceTests.java`, `meta-server/src/test/java/com/lol/meta/admission/AdmissionControllerTests.java` |
| game session token | `meta-server/src/test/java/com/lol/meta/session/GameSessionTokenServiceTests.java`, `meta-server/src/test/java/com/lol/meta/session/GameSessionTokenControllerTests.java` |
| internal credential | `meta-server/src/test/java/com/lol/meta/internal/InternalCredentialInterceptorTests.java` |
| settlement idempotency | `meta-server/src/test/java/com/lol/meta/settlement/SettlementServiceIdempotencyTests.java` |
| settlement rollback invariant | `meta-server/src/test/java/com/lol/meta/settlement/SettlementRollbackInvariantTests.java` |
| processing guard | `meta-server/src/test/java/com/lol/meta/settlement/SettlementProcessingGuardTests.java` |
| vertical slice | `meta-server/src/test/java/com/lol/meta/settlement/SettlementApiVerticalSliceTests.java` |

## Unity Player Client

| 영역 | 대표 테스트 |
| --- | --- |
| login / lobby / room scene controller | `client/unity_player_client/Assets/Tests/EditMode/LoginSceneControllerTests.cs`, `client/unity_player_client/Assets/Tests/EditMode/LobbySceneControllerTests.cs`, `client/unity_player_client/Assets/Tests/EditMode/RoomSceneControllerTests.cs` |
| TCP packet / network session | `client/unity_player_client/Assets/Tests/EditMode/PlayerTcpPacketTests.cs`, `client/unity_player_client/Assets/Tests/EditMode/PlayerNetworkSessionTests.cs` |
| RUDP packet / sender | `client/unity_player_client/Assets/Tests/EditMode/PlayerRudpPacketTests.cs`, `client/unity_player_client/Assets/Tests/EditMode/PlayerTcpNetworkConnectorTests.cs` |
| loot / inventory render surface | `client/unity_player_client/Assets/Tests/EditMode/PlayerDropMarkerRendererTests.cs`, `client/unity_player_client/Assets/Tests/EditMode/PlayerInventoryStatusRendererTests.cs` |
| standalone OAuth / config | `client/unity_player_client/Assets/Tests/EditMode/StandaloneOAuthLoginFlowTests.cs`, `client/unity_player_client/Assets/Tests/EditMode/Release0ClientConfigTests.cs` |

## Capacity / Stability Evidence

| 항목 | 공개 근거 |
| --- | --- |
| 100세션 인계 검증 | `scripts/release0/handoff_100_sessions_harness.py`, `scripts/release0/test_handoff_100_sessions_harness.py` |
| 부하 리포트 통과 기준 | `scripts/release1/capacity_report.py`, `scripts/release1/gate_config.json`, `scripts/release1/test_capacity_report.py` |
| 병렬 접속 probe / sweep wrapper | `scripts/release1/concurrent_capacity_probe.py`, `scripts/release1/concurrent_capacity_sweep.py` |
| artifact schema | `scripts/release1/schemas/run_config.schema.json`, `scripts/release1/schemas/summary.schema.json` |
| 670세션 로컬 진단 | raw private artifact는 공개 미러에서 제외. README에서는 안전 동접 수 주장이 아니라 진단 범위로만 언급 |

## 실행 명령

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

```bash
cd meta-server
./gradlew test
```

```bash
python3 -m unittest \
  scripts.release0.test_handoff_100_sessions_harness \
  scripts.release1.test_capacity_report \
  scripts.release1.test_concurrent_capacity_probe
```
