# 공개 제품 코드 기준

## 고정한 원본

- 저장소: [FASD92/Loot-of-Legends-V2](https://github.com/FASD92/Loot-of-Legends-V2)
- main SHA: `450ecb5ffbf5689adc3f71bddf398bc8678949d4`
- 공개 동기화 전 기준: `b5abe227de582130483ceab9b3dcb8477fec2284`

원본 Git 이력은 옮기지 않았습니다. 검토한 파일의 내용만 공개 브랜치에 적용했습니다.

## 포함한 범위

- CMake와 게임 서버 제품 코드
- battle continuity 계약과 중요 사건 저장 및 관련 C++ 테스트
- 연결별 RTT 기반 RUDP 재전송과 타이밍 테스트
- 현재 TCP/RUDP golden을 읽는 공개 load protocol client
- 기존 내용과 동일한 Meta 제품 소스와 Gradle build
- Unity C#과 asmdef 및 uGUI package lock과 companion meta

Meta 제품 소스와 build 파일은 동기화 전에도 원본 SHA와 byte 단위로 같아 diff가 없습니다.

## 제외한 범위

- private evidence admission 계약과 운영 배포 자료
- credential과 endpoint 및 원본 로그
- RAG 문서와 내부 Foundation 및 Plan
- Unity Presentation과 ThirdParty 미디어
- 자동 생성된 Unity Package Manager 설정

Unity 미디어는 별도 라이선스 검토 전까지 공개하지 않습니다. 따라서 현재 공개본은 Unity source import와 compile에 필요한 코드와 package identity만 제공하며 asset 기반 Unity 테스트 결과는 제공하지 않습니다.

과거 진단 브랜치의 `8ms / 4096 datagrams` 변경과 retained loot prune-before-push 수정은 현재 제품 main에 포함되지 않습니다. 사례 자료는 당시 실행을 설명하기 위해 별도 경로에 보존합니다.
