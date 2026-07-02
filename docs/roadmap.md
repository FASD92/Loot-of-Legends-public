# 로드맵과 현재 한계

이 문서는 README가 과장처럼 보이지 않도록 현재 완료 범위와 후속 범위를 분리합니다.

## 현재 공개 기준 완료

| 영역 | 상태 | 근거 |
| --- | --- | --- |
| C++ Room / Loot 권위 판정 | 구현 및 regression 포함 | `RoomManagerTests`, `ServerRoomIntegrationTests` |
| TCP gameplay path | 서버 통합 테스트 포함 | `ServerIntegrationTests`, `TcpPacketTests` |
| Custom RUDP foundation | packet, ACK, retransmit, duplicate guard, reliable event queue 포함 | `tests/protocol/` |
| Room Actor foundation | same-room single-writer, dispatcher, worker, metrics regression 포함 | `tests/core/RoomActorTests.cpp`, `tests/core/WorkerPoolTests.cpp` |
| Spring Meta boundary | auth/admission/session/settlement test slice 포함 | `meta-server/src/test/java/com/lol/meta/` |
| Unity Player Client source | login/lobby/room/network/RUDP/loot UI source와 EditMode tests 포함 | `client/unity_player_client/` |
| Capacity report contract | gate config, schema, evaluator, probe helper 포함 | `scripts/release1/` |

## 현재 한계

- README의 기준 gameplay path는 여전히 TCP Room/Loot path입니다.
- RUDP는 movement/reliable event foundation과 테스트가 중심이며, 모든 gameplay result transport가 RUDP로 이전됐다고 주장하지 않습니다.
- Room Actor / WorkerPool은 foundation과 regression이 포함되어 있지만, production runtime 전체가 multi-worker로 완전히 전환됐다는 뜻은 아닙니다.
- 670-session local diagnostic은 safe capacity 숫자가 아니라 failure stage를 분리하기 위한 진단 근거입니다.
- raw private experiment logs, internal planning, ADR, deploy 세부 파일은 public mirror에 포함하지 않습니다.
- Unity third-party asset package와 build output은 저장소에 포함하지 않습니다.

## 다음에 보강할 때 우선순위

1. README의 Playable Demo 섹션에 실제 빌드 링크와 실행 조건을 추가합니다.
2. Release1 capacity run artifact 중 public에 공개 가능한 요약만 선별해 `docs/test_matrix.md`에 연결합니다.
3. RUDP gameplay result surface를 넓히고, loss/jitter/reorder 기준의 soak evidence를 추가합니다.
4. production WorkerPool 연결 범위를 명확히 정하고, Room ownership policy를 문서와 테스트로 고정합니다.
5. Meta Server와 C++ server의 runtime settlement 연동 근거를 추가합니다.

## 포트폴리오 표현 원칙

- “최대 몇 명 가능”보다 “어떤 gate로 검증했는가”를 먼저 보여줍니다.
- 완료된 gameplay 권위 판정과 확장 foundation을 섞어 과장하지 않습니다.
- private docs를 공개하지 않는 대신, 공개 가능한 source/test/schema로 근거를 남깁니다.
- 실행 파일이나 영상은 README에 링크할 준비가 됐을 때만 추가합니다.
