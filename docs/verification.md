# Verification report

이 문서는 “코드/테스트가 존재한다”와 “이번 public branch에서 실행했다”를 분리한다.
결과는 2026-08-13, macOS arm64 worktree에서 수집했다.

## Environment

| Tool | Observed |
| --- | --- |
| CMake | 4.0.3 |
| Ninja | 1.13.2 |
| C++ compiler | Apple Clang 21.0.0 |
| Python | 3.14.6 |
| Java | OpenJDK 21.0.12 |
| Docker daemon | 29.3.1 |
| Unity Editor | 발견되지 않음 |

## Executed results

| Scope | Command | Result | Evidence boundary |
| --- | --- | --- | --- |
| Architecture checker tests | `python3 -m unittest discover -s scripts/architecture/tests -p 'test_*.py' -v` | **7/7 PASS** | checker의 허용/거부 회귀 |
| Architecture contract | `python3 scripts/architecture/check_architecture.py ...` | **PASS, 0 findings** | configured CMake target graph와 source include policy |
| C++ configure | `cmake --preset dev-debug` | PASS | build graph 생성 |
| C++ build | `cmake --build --preset dev-debug --parallel 4` | PASS | public C++ tree compile/link |
| C++ registered tests | `ctest --preset dev-debug --output-on-failure` | **46/46 PASS** | macOS local |
| Linux public CI | [main workflow run](https://github.com/FASD92/Loot-of-Legends-public/actions/runs/31663426685) | **PASS** | Ubuntu CMake/build/CTest에서 Linux `epoll` 경로 실행 |
| Load harness | `python3 -m unittest discover -s tools/load/tests -p 'test_*.py' -v` | **47/47 PASS** | runner/workload/classifier/sanitizer/package unit/contract |
| Local boundary fixture | `python3 tools/load/run_local_fixture.py ...` | **COMPLETE** | 10명·1 Room·2 cycle, package COMPLETE; `fixtureOnly=true`, `qualificationEligible=false`, `NOT_CLASSIFIED` |
| Fixture package | `python3 tools/load/verify_artifacts.py ...` | **COMPLETE** | missing/reason code 없음, checksum 검증 |
| Meta | `./gradlew --no-daemon --dependency-verification strict clean check` | **BUILD SUCCESSFUL** | Java 21, MySQL/Redis Testcontainers, formatting/compile/test |
| Claim verifier regression | `python3 -m unittest scripts/public/test_verify_portfolio_claims.py -v` | **1/1 PASS** | ignored/untracked 파일 배제, stale link/count/privacy failure 검증 |

첫 sandboxed CTest는 loopback bind 권한이 없어 network test 4개가 실행 환경 오류로 실패했다.
동일 build와 suite를 loopback 권한이 있는 환경에서 재실행한 최종 결과가 46/46 PASS다.

## Pending or unavailable

| Scope | Status | Reason / consequence |
| --- | --- | --- |
| Unity EditMode/PlayMode | NOT RUN | Unity Editor 없음 |
| Official capacity | NOT PROVEN | qualifying public run/evidence package 없음 |

CI 파일은 [public-integrity workflow](../.github/workflows/public-integrity.yml)에서 확인할 수 있다.
GitHub run과 local 실행 결과를 서로 대체하지 않고 각각 위 표에 기록한다.

## Reproduce

### Static claim and privacy boundary

```bash
python3 -m unittest scripts/public/test_verify_portfolio_claims.py -v
python3 scripts/public/verify_portfolio_claims.py
```

Verifier는 `git ls-files`만 사용한다. ignore된 local 파일을 공개 근거로 세거나 검사 결과를
왜곡하지 않는다. 이 PASS는 source/test path, Markdown link, count marker와 대표 leak pattern의
정적 일치만 뜻하며 테스트 실행을 뜻하지 않는다.

### C++ product and CTest

Prerequisites: CMake 3.28+, Ninja, C++20 compiler, libcurl, OpenSSL, pthread.

```bash
cmake --preset dev-debug
cmake --build --preset dev-debug --parallel 4
python3 -m unittest discover -s scripts/architecture/tests -p 'test_*.py' -v
python3 scripts/architecture/check_architecture.py \
  --source-root . \
  --build-dir build/dev-debug \
  --contract contracts/architecture/target-graph.v1.json \
  --report build/dev-debug/architecture-report.json
ctest --preset dev-debug --output-on-failure
```

### Load harness

```bash
python3 -m unittest discover -s tools/load/tests -p 'test_*.py' -v
```

실제 TCP/UDP/RUDP 기능 fixture는 [`tools/load/README.md`](../tools/load/README.md)를 따른다.
그 결과는 `fixtureOnly=true`이며 capacity 측정이 아니다.

### Meta settlement

Prerequisites: Java 21, 실행 중인 Docker daemon, MySQL/Redis Testcontainers image pull 가능 환경.

```bash
cd meta-server
./gradlew --no-daemon --dependency-verification strict clean check
```

### Unity

`client/unity`를 해당 ProjectVersion의 Unity Editor로 열고 EditMode/PlayMode tests를 실행한다.
Editor나 CI result가 없는 상태에서는 test source 수를 실행 결과로 표현하지 않는다.
