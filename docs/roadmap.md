# 로드맵과 현재 한계

이 문서는 public README의 상태 경계를 보완합니다. 목적은 많이 나열하는 것이 아니라, 현재 검증된 것과 후속 통합 범위를 분리해서 보여주는 것입니다.

## 완료된 핵심

| 영역 | 상태 | 근거 |
| --- | --- | --- |
| TCP Room / Session / Loot MVP | 완료 | RoomManager 단위 테스트와 TCP 통합 테스트 |
| 서버 권한 루팅 판정 | 완료 | 동일 Drop claim, duplicate claim, overweight rejection 검증 |
| Debug CLI 시연 | 완료 | CLI command parsing과 실제 서버 흐름 재현 |
| SettlementResult 계약 | 완료 | C++ 서버의 payload 생성과 반복 요청 멱등 응답 검증 |
| Unity item/loot smoke | smoke 완료 | server-origin DropListSnapshot, same-drop LootResolved/LootRejected, winner InventorySnapshot, marker hide manual Full PASS |

## 확장 중인 영역

| 영역 | 현재 단계 | 후속 통합 |
| --- | --- | --- |
| Spring Meta Server | MySQL / Redis 기반 정산 테스트 세로 슬라이스 | C++ 서버에서 runtime HTTP 호출 |
| Custom RUDP | protocol/unit layer, Reliable Ordered event queue, movement dispatch, StateSnapshot render 검증 | broader gameplay transport policy와 soak/stress |
| Room Actor / WorkerPool | primitive/regression, TCP inline actor pump, RUDP movement dispatch 경계 검증 | production WorkerPool thread 연결 |
| Linux epoll runtime | `NetworkEventLoop` abstraction, Linux `EpollEventLoop`, stress target 추가 | cross-platform production hardening |
| Unity Thin Client | TCP debug session, RUDP Hello/Move, StateSnapshot render, item/loot smoke UI | automated Unity Play Mode smoke와 production client UX |
| Observability / stress | metric primitive와 epoll stress target | 100-client stress/soak 결과 기록 |

## 현재 한계

- TCP gameplay runtime은 현재 2인 Room 기준입니다.
- 10인 ClickLoot 경합은 RoomActor regression으로 검증했지만, production room size 정책은 후속 범위입니다.
- Meta Server는 별도 모듈에서 정산 API와 DB 멱등성을 검증했지만, C++ 서버와 runtime HTTP로 연결되지는 않았습니다.
- RUDP movement dispatch와 StateSnapshot render는 연결됐지만, 전체 gameplay transport 전환과 loss/jitter/reorder soak는 후속 범위입니다.
- production `Core::Server`는 TCP inline actor pump를 사용하며, WorkerPool thread runtime 연결은 후속 범위입니다.
- Unity Thin Client는 smoke/evidence client이며, automated Play Mode smoke와 100-client stress/soak test는 아직 예정입니다.

## 다음 통합 방향

1. C++ 서버의 `SettlementResult`를 Spring Meta Server로 실제 전송합니다.
2. RUDP movement 외 gameplay result surface와 outbound transport policy를 확장합니다.
3. RUDP Reliable Ordered event pending queue의 ACK consume / retransmission runtime policy를 강화합니다.
4. production WorkerPool thread 연결 전 RoomManager ownership / concurrency policy를 확정합니다.
5. Unity Play Mode 자동 smoke와 stress/soak test로 외부 재현성을 확장합니다.

## 포트폴리오에서 강조할 점

- 완성된 핵심은 C++ TCP 서버 권한 루프와 Debug CLI 재현성입니다.
- Unity Thin Client는 production client가 아니라 서버 권한 결과를 눈으로 확인하는 smoke/evidence surface입니다.
- 확장 영역은 미완성으로 숨기지 않고, 어디까지 테스트로 고정됐는지 공개합니다.
- DB, RUDP, WorkerPool은 hot path를 무리하게 섞지 않고 단계적으로 통합합니다.
