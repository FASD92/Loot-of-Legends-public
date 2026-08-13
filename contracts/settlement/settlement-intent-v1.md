# Settlement Intent v1 Contract

Status: `Accepted 1.0.1`

정산(intent) 값은 `lol::settlement` module이 소유하는 순수 불변 값이며
`lol_shared_kernel`만 참조한다. transport, HTTP, DB, Redis, storage,
observability 구현은 참조하지 않는다.

## 값

- `SettlementId`: 16-byte ordered bytes.
- `SettlementBatchId`: 16-byte ordered bytes.
- `CanonicalPayloadHash`: SHA-256 결과 32-byte.
- `BattleOutcome`: `MonsterDefeated=1`, `CombatTimeout=2`,
  `CancelledNoActiveParticipants=3`.
- `ParticipantExitStatus`: `TerminalPresent=1`, `TerminalExited=2`.
- `ItemDelta{uint64 itemId, uint64 quantity}`.
- `ResultCommittedAt{uint64 unixEpochMilliseconds, uint64 monotonicNanoseconds}`.
- participant source: AccountId, terminal exit status, item deltas, final
  asset value.
- batch source: RoomId, BattleInstanceId, outcome, `uint16` catalogVersion,
  committed time, captured participant sources.

intent/batch는 const access만 제공하는 불변 class이고 equality를 가진다.
factory는 `createSettlementIntentBatch` 하나이며 `optional<SettlementIntentBatch>`
를 돌려준다. `canonicalPayload(intent)`는 payload bytes를 돌려주는 순수
함수다. SHA-256 구현은 `SettlementIntent.cpp`에 private하게 둔다.

## Validation

- RoomId, BattleInstanceId, catalogVersion은 nonzero.
- participants는 2..10.
- AccountId는 unique.
- itemId는 nonzero이고 participant 내에서 unique, quantity는 positive.
- participant의 item count는 u16에 맞아야 한다.
- CombatTimeout은 모든 participant가 item이 없고 final asset value가 0이다.
- CancelledNoActiveParticipants는 모든 participant가 TerminalExited여야 한다.
  frozen positive item delta와 final asset value를 보존하며, item/value가 0인
  participant도 허용한다.
- item delta는 itemId 오름차순, intent는 AccountId raw bytes 오름차순으로
  정규화한다.
- MonsterDefeated는 item/value가 비어 있어도 된다. catalog/value 검증은
  Battle이 소유한다.

## Encoding

모든 integer는 unsigned big-endian이다. ASCII에는 NUL/length prefix가 없다.

```text
SettlementId = first16(SHA256(
  ASCII("settlement-v1") || RoomId:u64 || BattleId:u64 || AccountId:16))

SettlementBatchId = first16(SHA256(
  ASCII("settlement-batch-v1") || RoomId:u64 || BattleId:u64))

canonical payload =
  ASCII("settlement-intent-v1") # 20
  || SettlementId:16
  || AccountId:16
  || RoomId:u64
  || BattleId:u64
  || Outcome:u8
  || ExitStatus:u8
  || CatalogVersion:u16
  || ItemCount:u16
  || repeated(ItemId:u64 || Quantity:u64) in ItemId order
  || FinalAssetValue:u64
  || WallUnixMilliseconds:u64
  || MonotonicNanoseconds:u64

CanonicalPayloadHash = SHA256(canonical payload)
```

hash는 자신의 payload에 포함되지 않는다. SessionId, nickname, rank, winner,
storage status는 없고 mutable API도 없다.

## Golden vectors

`golden/settlement-intent-v1.json`은 semantic inputs와 expected
batch/id/payload/hash를 담는다. C++ 테스트는 같은 vector를 hardcode로 증명하며
JSON parser 의존성이 없다. 1.0.1은 기존 outcome 1/2 vector bytes를 변경하지
않고 cancellation outcome byte 03 vector만 추가한다.
