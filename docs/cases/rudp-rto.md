# 지연 변화에 맞춘 RUDP 재전송

RTT가 늘어나도 새 패킷이 매번 200ms에 재전송되면, 재전송된 패킷의 ACK를 제외하는 Karn 규칙 때문에 새 RTT를 학습하지 못할 수 있습니다. 패킷별 backoff 외에 연결에도 대기 시간을 보존하도록 바꿨습니다.

## 구현

[RttEstimator](../../game-server/platform/transport-rudp/src/RttEstimator.cpp)는 연결별 SRTT와 RTTVAR로 재전송 대기를 계산합니다. [ReliableQueue](../../game-server/platform/transport-rudp/src/ReliableQueue.cpp)는 실제 송신 성공 시각, ACK와 패킷별 backoff를 관리합니다.

[RudpBindingRegistry](../../game-server/platform/transport-rudp/src/RudpBindingRegistry.cpp)는 timeout을 받으면 다음 새 패킷의 최소 대기를 늘립니다. 현재 recovery revision으로 시작한 새 패킷의 유효 ACK를 받으면 RTT를 갱신하고 이 대기를 해제합니다. 큐가 비어도 학습 상태는 남으며, 연결 교체나 만료 시 폐기합니다. 이미 대기 중인 패킷의 타이머는 바꾸지 않습니다.

예를 들어 RTT 60ms를 학습한 뒤 실제 RTT가 300ms로 늘면, 200ms timeout 뒤 다음 새 패킷에 400ms 대기를 줍니다. 이 패킷은 재전송 전에 ACK를 받아 새 RTT를 학습할 수 있습니다. [결정적 시계 테스트](../../tests/transport-rudp/RudpTimingTests.cpp)로 이 경로와 옛 ACK, 중복 timeout, 연결 교체를 검사합니다.

초기 대기는 200ms, RTO 범위는 200~1,000ms, 최대 전송은 5회이고 enqueue 후 5초에 만료합니다. [현재 계약](../../contracts/protocol/rudp-transport-config.v1.json)은 C++ 서버 reliable sender를 대상으로 합니다.

## 최종 초기 200ms 정책의 측정

같은 Windows PC에서 WSL2 GameServer와 Windows Python 클라이언트 2개를 실행했습니다. 매번 공격 12개를 1초 간격으로 보내며 실행 12초와 drain 2초를 사용했습니다. 첫 3초 뒤 reliable 패킷 27개에서 센 실행별 서버 재전송입니다.

| 네트워크 조건 | 1회차 | 2회차 | 3회차 |
| --- | ---: | ---: | ---: |
| 추가 지연 없음 | 0 | 0 | 0 |
| 왕복 300ms 추가 | 0 | 0 | 0 |
| 지연 변동 | 1 | 1 | 1 |
| 지정 패킷 첫 복사본 유실 | 6 | 6 | 6 |

왕복 300ms 조건에서 초기 3초까지 포함한 재전송은 매번 2회였습니다.

측정은 recovery가 있는 초기 1,000ms 구현과 초기 200ms 구현을 4조건에서 각 3회 비교한 24회 batch입니다. 전체 24회 모두 공격 응답 12/12, 연결 2개 생존, 비교 패킷 전체 ACK, 클라이언트 timeout 0과 서버 exit 0을 확인했습니다. 최종 초기 200ms 구현만 24회 실행한 것은 아닙니다.

앞선 별도 batch의 fixed 200ms와 adaptive 1,000ms + recovery 비교에서는 왕복 300ms 조건이 27→0, 지연 변동이 18→1이었습니다. 이 값을 최종 초기 200ms와 fixed 200ms의 직접 비교로 합치지 않습니다.

## 근거

- 원본 보고서 보존 커밋: `450ecb5ffbf5689adc3f71bddf398bc8678949d4`
- 보고서: `2026-09-12-rudp-rto-bootstrap200-verification.md`
- 최종 측정 source fingerprint: `804ba4f4082ea3d8a697bda930db261755e4acdcb9dda126c32f8a0de080b601`
- 최종 초기 200ms binary SHA-256: `b5c8535923e069b36f3c42bde1336d2c2350ddcce218a2b9ea80709076c93467`

당시 측정 구현과 최종 병합 뒤 서버 진입 검사는 서로 다른 실행입니다. 재전송 횟수는 전송 지연이나 서버 수용량을 뜻하지 않습니다.

[사례 목록](README.md)
