# 공개 범위와 남은 검증

## 제품 코드와 사례 기록

루트의 빌드 가능한 제품 코드는 V2 main `450ecb5ffbf5689adc3f71bddf398bc8678949d4`를 기준으로 동기화했습니다. [후속 사례](cases/README.md)는 2026-08-23부터 2026-09-12까지 서로 다른 SHA에서 실행한 자료입니다. 제품 코드를 최신화해도 과거 측정의 source는 바꾸지 않았습니다.

| 항목 | 공개 자료의 범위 |
| --- | --- |
| C++ 제품 코드 | V2 main 450ecb5 기준 |
| Meta 제품 코드 | 원본과 byte-identical |
| Unity 코드 | C#과 asmdef 및 uGUI package identity 공개 |
| Unity 미디어 | 라이선스 검토 전 Presentation과 ThirdParty asset 제외 |
| Room 실행자 | RoomExecutionCell |
| CTest | 현재 공개 후보 63개 등록, 중단 전 51개와 재개 후 12개 PASS |
| 수신 상한 변경 | 당시 변경 선언과 전후 측정값 공개 |
| 메모리 정리 | 진단 브랜치 함수와 회귀 테스트 및 전후 RSS 공개 |
| 5,250세션 | 지정된 진단본의 60분 실행 요약과 RSS 공개 |
| 전투 복구 | 중요 사건 저장의 서버 진입 검사와 기존 동일 호스트 2-client 실행 및 DB 집계 공개 |
| 현재 공개 빌드의 동시접속 수용량 | 별도 부하로 입증하지 않음 |
| 다중 호스트 장애 전환 | 위 복구 실험에 포함되지 않음 |
| 모든 race 제거 | 주장하지 않음 |
| Unity 실행 | asset closure 제외로 실행하지 않음 |

RUDP의 ACK와 application CommandId 중복 제거는 별도 계층입니다. 일반적인 ordered delivery나 exactly-once network delivery를 보장한다고 쓰지 않습니다.

## 부하 기록

[5,250세션 기록](cases/capacity-5250.md)은 과거 진단 실행입니다. 동일 source의 제한된 관측으로 반복 재현성이나 서비스 SLA를 주장하지 않습니다. 이전 source에서 발생한 실패와 같은 source의 실패도 구분합니다.

공개용 JSON은 원문에서 선택한 필드를 담습니다. RSS TSV는 원본 저장소 파일과 byte 단위로 같습니다. 전체 운영 설정과 raw 로그는 공개하지 않았습니다. JSON과 RSS를 확인하는 것과 동일 실행 환경에서 결과를 재현하는 것은 다릅니다.

진단 실행의 instance type과 실제 DB 정산 부하 포함 여부는 공개 provenance만으로 확정하지 못했습니다. 공개 기록에는 UNKNOWN으로 남겼습니다.

## 기존 10-player fixture

Local fixture는 실제 game-server의 TCP/UDP/RUDP 처리와 합성 loopback Meta 및 두 번의 loot race cycle을 기능적으로 연결합니다. steady-state 처리량이나 포화 지점 및 패킷 장애 내성을 측정하는 실험이 아닙니다. qualificationEligible=false와 NOT_CLASSIFIED를 유지합니다.

## 공개 검사

[기존 검사기](../scripts/public/verify_portfolio_claims.py)는 Git-tracked 파일의 링크와 필수 경로 및 테스트 수 표기와 대표적인 비공개 정보 패턴을 검사합니다.

[사례 기록 검사](../scripts/public/test_case_records.py)는 공개 파일 목록의 해시와 측정 조건 및 RSS 기울기와 DB 집계의 관계를 검사합니다. 어느 검사도 전체 보안 감사나 개인 기여 입증 또는 성능 재현을 대신하지 않습니다.
