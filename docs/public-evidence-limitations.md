# Public evidence limitations

다음 내용은 현재 public tree만으로 증명할 수 없거나 실제 구현과 다르므로 주장하지 않는다.

| Claim | Status | 필요한 추가 근거 |
| --- | --- | --- |
| C++17 implementation | False for this snapshot | tracked CMake 기준 C++20 |
| `RoomActor` runtime | False for this snapshot | 실제 명칭/구현은 `RoomExecutionCell` |
| 1,268 tests | Stale | 현재 tree와 무관한 과거 public 합계; 재사용하지 않음 |
| RUDP ordered delivery | Not implemented as a general guarantee | reorder buffer/ordered delivery contract와 회귀 필요 |
| exactly-once network delivery | Not claimed | network retry와 application idempotency는 별도 |
| production-grade RUDP under loss/jitter/reorder | Not proven | 공개 impairment/soak run과 immutable result 필요 |
| 동시접속, TPS, p95/p99 latency 또는 safe capacity 수치 | Not proven | clean exact-SHA qualifying run과 sanitized evidence package 필요 |
| cloud/production deployment stability | Not public evidence | 공개 가능한 deployment/runtime evidence 필요 |
| 모든 race condition 제거 | Not provable | 현재는 per-room writer와 특정 gateway/terminal race 회귀만 증명 |
| Java settlement의 임의 장애 전반 exactly-once | Not claimed | 현재 근거는 same-ID/hash replay, conflict, transaction rollback/apply-once 경계 |
| Unity tests passing on this branch | Not run | matching Unity Editor 또는 public CI result 필요 |
| Linux `epoll` runtime passing on this branch | Not yet observed | push 이후 Ubuntu workflow 결과 또는 별도 Linux 실행 필요 |

## What the public load fixture proves

Local 10-player fixture는 실제 game-server process의 TCP/UDP/RUDP 경계, 합성 loopback Meta,
두 번의 loot race cycle과 evidence package 생성을 기능적으로 연결한다. 다음은 증명하지 않는다.

- production Meta/provider/network
- 시간 기반 steady-state throughput
- resource saturation point
- packet impairment tolerance
- official release/capacity gate 통과

그래서 fixture 결과는 항상 `qualificationEligible=false`, `NOT_CLASSIFIED`로 남긴다.

## What the verifier proves

[`verify_portfolio_claims.py`](../scripts/public/verify_portfolio_claims.py)는 Git-tracked tree에서
문서 링크, 핵심 source/test path, 선언 count marker와 대표 private leak pattern을 검사한다.
완전한 security scanner, authorship proof, test runner 또는 performance verifier는 아니다.
