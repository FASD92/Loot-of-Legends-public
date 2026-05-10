# Loot of Legends

Loot of Legends는 TCP 기반 멀티플레이 게임 서버 프로토타입입니다. 세션 관리, 방 생성과 입장, 전투 시작 이벤트, 몬스터 처치, 드롭 아이템, 루팅 소유권 판정, 세션 종료 정산 응답까지 서버 쪽 핵심 흐름을 C++17로 구현했습니다.

이 저장소는 이력서와 포트폴리오 검토를 위한 공개용 소스 미러입니다.

## 기술 스택

- C++17
- CMake
- GoogleTest
- POSIX 소켓

## 주요 구현 범위

- TCP 리스너와 패킷 직렬화/역직렬화
- 클라이언트 세션 생성, 만료, 제거
- 방 생성, 입장, 퇴장, 준비 상태 관리
- 양쪽 플레이어 준비 완료 시 전투 시작 이벤트 전파
- 몬스터 생성, 처치, 드롭 목록 동기화
- 클릭 루팅 요청 처리와 인벤토리 무게 제한 검증
- `finish_session` 요청에 대한 멱등 정산 결과 생성
- 서버 재현용 Debug CLI 클라이언트
- 단위 테스트와 로컬 통합 테스트

## 프로젝트 구조

```text
.
├── CMakeLists.txt
├── client/
│   ├── CMakeLists.txt
│   └── debug_cli/
├── server/
│   ├── CMakeLists.txt
│   └── src/
│       ├── Core/
│       ├── Game/
│       ├── Net/
│       └── Util/
└── tests/
    ├── CMakeLists.txt
    ├── client/
    ├── core/
    └── protocol/
```

## 빌드

```bash
cmake -S . -B build
cmake --build build
```

## 실행

서버:

```bash
./build/server/lol_server 40000
```

포트를 지정하지 않으면 기본값으로 `40000`을 사용합니다.

Debug CLI:

```bash
./build/client/lol_debug_cli
```

## 테스트

```bash
ctest --test-dir build --output-on-failure
```
