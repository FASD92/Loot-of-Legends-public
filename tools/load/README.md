# Public load-validation harness

Python 표준 라이브러리만으로 실제 game-server TCP/UDP/RUDP 경계를 구동한다. Parent
runtime은 bounded worker·heartbeat·phase 전이를 관리하고, workload는 명령을 생성하며,
classifier와 evidence package는 실행 후 별도 단계에서 판정·검증한다.

## Local 10-player fixture

먼저 C++ server를 빌드한 뒤 새 출력 디렉터리를 지정한다.

```bash
python3 tools/load/run_local_fixture.py \
  --profile tools/load/profiles/release-functional-10p-v1.json \
  --fixture-server build/dev-debug/game-server/app/composition-root/lol_game_server \
  --output /tmp/lol-public-functional \
  --run-id local-functional
```

이 fixture는 실제 game-server entry point와 합성 loopback Meta를 사용해 10명·1개
Room·2개 loot cycle을 통과한다. 결과에는 `fixtureOnly=true`,
`qualificationEligible=false`, `evaluationStatus=NOT_CLASSIFIED`가 기록된다. 따라서
성능·용량 수치의 근거가 아니라 product boundary와 evidence pipeline의 기능 검증이다.

## Components

- `loot_load/runner/runtime.py`: bounded multiprocessing, heartbeat, phase control
- `loot_load/workload/loot_race.py`: TCP/RUDP 명령 스케줄과 terminal correlation
- `loot_load/closeout/classifier.py`: `ABORTED`, `INVALID`, `FAIL`, `PASS` 분리
- `loot_load/evidence/package.py`: no-clobber package, sanitization, SHA-256 verification

단위 검증은 저장소 루트에서 실행한다.

```bash
python3 -m unittest discover -s tools/load/tests -p 'test_*.py' -v
```

공식 capacity 결과와 raw 운영 evidence는 이 공개 저장소에 포함하지 않는다.
