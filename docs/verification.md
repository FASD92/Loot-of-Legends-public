# 공개본 검증 기록

2026-09-07에 공개 저장소 checkout만 사용했습니다. 제품 코드의 원본 기준은 `17c6d7fc0996e389fdf025de8a926974f9186b79`입니다.

## 환경

| 도구 | 확인값 |
| --- | --- |
| macOS | arm64 |
| CMake | 4.0.3 |
| Ninja | 1.13.2 |
| C++ compiler | Apple Clang 21.0.0 |
| Python | 3.14.6 |
| Java | OpenJDK 21.0.12 |
| Docker daemon | 29.7.2 |

## 실행 결과

| 범위 | 결과 |
| --- | --- |
| CMake configure | PASS |
| 전체 C++ build | 205 steps PASS |
| CTest | 60/60 PASS, 240.29초 |
| architecture unit | 7/7 PASS |
| architecture graph | 17 targets / 46 edges / 236 source files / 0 findings |
| Load 도구 unit | 47/47 PASS |
| Meta Gradle check | BUILD SUCCESSFUL, 8 tasks |
| 공개 claim unit | PASS |
| 사례 기록 unit | PASS |
| tracked link와 secret 검사 | PASS |

## 실행 명령

```bash
cmake --preset dev-debug
cmake --build --preset dev-debug --parallel 4
ctest --preset dev-debug --output-on-failure

python3 -m unittest discover -s scripts/architecture/tests -p 'test_*.py' -v
python3 scripts/architecture/check_architecture.py \
  --source-root . \
  --build-dir build/dev-debug \
  --contract contracts/architecture/target-graph.v1.json \
  --report build/dev-debug/architecture-report.json

python3 -m unittest discover -s tools/load/tests -p 'test_*.py' -v

cd meta-server
./gradlew --no-daemon --dependency-verification strict clean check
cd ..

python3 -m unittest scripts/public/test_verify_portfolio_claims.py -v
python3 -m unittest scripts/public/test_case_records.py -v
python3 scripts/public/verify_portfolio_claims.py
```

첫 Meta 실행은 sandbox가 사용자 Gradle cache lock을 열지 못해 task 시작 전에 종료됐습니다. 같은 checkout과 명령을 Gradle cache 접근이 가능한 환경에서 다시 실행했고 `BUILD SUCCESSFUL`을 확인했습니다.

## 실행하지 않은 항목

Unity Presentation과 ThirdParty asset은 공개 범위에서 제외했습니다. Unity asset test와 Standalone E2E는 실행하지 않았습니다. 과거 5,000세션 비교와 5,250세션 부하 및 Unity 복구와 MySQL 대조도 다시 실행하지 않았습니다.

이 문서의 C++과 Meta 및 Python 결과는 현재 공개 checkout의 검증입니다. [사례 기록](cases/README.md)의 성능과 복구 결과는 각 문서에 적힌 과거 source SHA의 실행입니다.
