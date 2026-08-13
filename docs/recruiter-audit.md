# Adversarial recruiter audit

감사 기준은 “채용담당자가 30초~3분 안에 주장과 코드를 연결할 수 있는가”다. 비교 대상은
변경 전 public tree와 현재 Git-tracked public tree다.

## Findings and disposition

| 감사 항목 | 기존 public 상태 | 판정 | 처리 |
| --- | --- | --- | --- |
| 첫 화면 | 기술 목록과 오래된 테스트 합계가 먼저 보이고 어려운 문제/근거 연결이 늦음 | High | Problem → Architecture → What I built → Evidence → Measured results → How to verify 순서로 재작성 |
| 언어 버전 | `C++17` 주장 | False for current tree | tracked CMake의 `CMAKE_CXX_STANDARD 20`에 맞춰 C++20으로 정정 |
| concurrency 명칭 | `RoomActor`와 foundation 수준 설명 | Stale | 실제 product path의 `RoomExecutionCell` + `WorkerPool` single-writer/parallel-cell 테스트로 교체 |
| architecture | 문서상 queue → dispatcher → actor였지만 기존 dispatcher가 per-room queue를 소유했고, 현재 runtime과도 다른 세대 | Misleading | composition root → gateway → Cell → battle → settlement 실제 연결로 교체 |
| RUDP | “순서 보장” 표현 | Unproven for current tree | ACK bitmap/제한 재전송과 application replay/conflict만 주장; ordered delivery를 명시적으로 제외 |
| 테스트 수 | C++ 659 + Meta 221 + Unity 388 = 1,268 | Stale/incorrect | 기존 tracked tree의 Meta 선언은 199, 합계는 1,246이었다. 현재 tree와 무관한 합계는 폐기 |
| 부하 표현 | `100-session` harness가 실행 artifact처럼 읽힘 | Unverifiable result | harness 구현과 실행 결과를 분리하고 official capacity를 `NOT PROVEN`으로 표시 |
| Quick start | compiler/library, CMake 3.28/Ninja, Java 21, Docker/Testcontainers 조건 누락 | Not reproducible enough | preset 기반 C++ 명령과 prerequisites, Meta Docker 조건을 문서화 |
| local links | 주요 local 경로는 당시 tree에 존재했지만 현행 구현을 가리키지 않음 | Semantically stale | 새 evidence 링크를 현재 source/test에 직접 연결하고 tracked-tree verifier로 존재 검사 |
| 중복 | README, architecture, protocol, RoomActor, test matrix가 같은 설명/숫자를 반복 | Drift risk | README는 recruiter path, architecture는 semantics, verification은 실행 결과, limitations는 비주장만 소유 |
| public/private 경계 | 제외 원칙은 있었지만 자동화가 없고 mirror 후보에 개인 경로·운영 topology가 섞일 수 있었음 | High | allowlist mirror, 합성 fixture 정규화, 금지 경로/credential/absolute-path verifier와 CI 추가 |

## Before / after

| Before | After |
| --- | --- |
| 오래된 구현 세대를 현행 포트폴리오처럼 읽어야 함 | 현재 public-safe product slice |
| 기술 키워드와 큰 테스트 합계 중심 | 어려운 문제 6개를 구현 파일과 회귀 파일에 직접 연결 |
| RUDP transport와 gameplay idempotency가 혼재 | ACK/retry, movement newest-only, `CommandId` replay/conflict를 분리 |
| `RoomActor` foundation 설명 | 실제 `RoomExecutionCell` single-writer와 multi-cell parallelism |
| load harness 존재가 capacity 결과처럼 보일 수 있음 | 실행 상태와 `NOT PROVEN` capacity를 명시 |
| 수동 문서 점검 | `git ls-files` 기반 claim/link/count/privacy verifier + CI |

## Private/public boundary applied

Public mirror에는 C++/Java/Unity product code, protocol/settlement contracts, deterministic load
harness와 검증 테스트만 포함한다. 내부 authority/status/handoff, plans/reviews/runbooks,
infra/deploy, provider topology, raw run artifact, credential, local absolute path와 generated
output은 포함하지 않는다.

Verifier는 대표적인 금지 경로, private key/access-token 형식, credential-bearing URL,
개인 absolute path와 내부 hostname을 Git-tracked tree에서 거부한다. 이는 완전한 secret
scanner가 아니라 이 저장소의 public boundary 회귀 방지 장치다.

## Audit result

현재 README의 핵심 역량은 모두 tracked implementation과 test source에 연결된다. 실제 실행
결과와 static source registration은 [verification report](verification.md)에서 분리한다.
아직 공개로 증명하지 못한 내용은 [public evidence limitations](public-evidence-limitations.md)에
남겨 과장된 빈칸을 문장으로 채우지 않는다.
