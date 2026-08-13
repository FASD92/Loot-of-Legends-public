# Outbox Journal Contract v1

Status: Accepted

Contract version: 1.0.0

Canonical owner: `lol_settlement_storage`

## 1. 목적과 경계

이 계약은 `SettlementIntentBatch`를 local persistent volume에 crash-safe하게
기록하고 재시작 때 publish 가능한 batch를 복구하는 binary journal 형식을
고정한다. Journal은 append-only이며 하나의 battle batch를 섞지 않고
`Intent...BatchCommit` 순서로 기록한다.

Journal은 settlement 의미, Room/Battle 상태, Meta 적용 상태를 변경하지 않는다.
`Intent` payload는
[`settlement-intent-v1.md`](settlement-intent-v1.md)의 canonical payload bytes를
그대로 보존한다.

## 2. 공통 encoding

- 모든 정수는 unsigned big-endian이다.
- byte offset은 segment 시작부터 0-based이다.
- `journalSequence`는 1부터 시작하며 segment 세대를 넘어 엄격히 증가한다.
- `recordLength`는 magic부터 CRC까지 포함한 전체 record byte 수다.
- 고정 envelope는 payload 전 56 bytes, CRC까지 포함하면 60 bytes다.
- `recordLength == 60 + payloadLength`여야 한다.
- `payloadHash`는 payload bytes의 SHA-256이다.
- `crc32`는 CRC field 전까지의 envelope와 payload 전체에 대한 IEEE CRC-32다.
  Polynomial은 `0xEDB88320`, initial/final XOR는 `0xFFFFFFFF`다.

## 3. Record envelope

| Offset | Size | Field | Value |
|---:|---:|---|---|
| 0 | 4 | magic | `0x4C4F4F32` (`LOO2`) |
| 4 | 2 | formatVersion | `1` |
| 6 | 2 | recordType | `Intent=1`, `BatchCommit=2`, `Retired=3` |
| 8 | 4 | recordLength | `60 + payloadLength` |
| 12 | 8 | journalSequence | nonzero, strictly increasing |
| 20 | 4 | payloadLength | payload byte count |
| 24 | 32 | payloadHash | `SHA256(payload)` |
| 56 | N | payload | record-type-specific bytes |
| 56+N | 4 | crc32 | `CRC32(bytes[0..56+N))` |

Parser는 available bytes를 확인하기 전에 `recordLength`나 `payloadLength`를
신뢰하여 allocation하지 않는다. Unknown magic, version, type, length, sequence,
payload hash 또는 CRC는 corrupt record다.

## 4. Record payloads

### 4.1 Intent (`recordType=1`)

Payload 전체가 canonical `SettlementIntent` bytes다. 별도 길이 prefix나
batch ID를 덧붙이지 않는다. Writer는 한 batch의 Intent를 canonical participant
순서로 연속 기록한다.

### 4.2 BatchCommit (`recordType=2`)

```text
SettlementBatchId[16]
intentCount u16
repeat intentCount times, append order:
  intentJournalSequence u64
  intentPayloadHash[32]
```

- `intentCount`는 2..10이다.
- tuple 순서는 바로 앞의 연속된 Intent record 순서와 같아야 한다.
- tuple sequence/hash가 하나라도 다르면 batch는 committed가 아니다.
- Intent record 사이에 다른 type이나 다른 batch가 끼어들 수 없다.
- 한 `SettlementBatchId`에는 정확히 하나의 valid `BatchCommit`만 허용한다.

Writer는 모든 Intent와 하나의 BatchCommit을 append한 뒤 `fdatasync`를 정확히
한 번 호출한다. 그 호출이 성공한 뒤에만 batch를 `DurablyQueued`로 보고한다.

### 4.3 Retired (`recordType=3`)

```text
SettlementBatchId[16]
batchCommitJournalSequence u64
```

Retired는 해당 valid committed batch 뒤에만 올 수 있다. ID와 commit sequence가
모두 일치해야 하며, valid Retired가 있는 batch는 재시작 publisher 대상이 아니다.
Retirement는 기존 record를 덮어쓰지 않는다.

## 5. Recovery

Recovery는 segment 순서와 record 순서대로 수행한다.

1. 완전하고 검증된 `Intent...BatchCommit`만 committed batch로 복구한다.
2. EOF가 record 중간 또는 commit 전이면 마지막 incomplete batch의 첫 Intent부터
   끝까지 quarantine copy로 보존하고 active segment에서 잘라낸다. 그 batch는
   publish하지 않는다.
3. hash/CRC/format/sequence가 잘못된 tail은 첫 corrupt record부터 끝까지
   quarantine하고 storage health를 degraded로 만든다. corrupt tail 앞의 valid
   committed batch는 보존한다.
4. committed batch 내부를 검증할 수 없으면 그 batch 시작부터 quarantine하며
   fake durability completion을 만들지 않는다.
5. valid Retired는 일치하는 committed batch에만 적용한다.
6. compaction은 unretired committed batch만 새 generation에 복사하고 새 manifest를
   atomic replace한 뒤 이전 generation을 회수한다.

Quarantine은 원본 tail bytes와 source generation/offset을 보존한다. Evidence에는
credential이나 token을 넣지 않으며 public artifact에는 별도 sanitized digest만
노출한다.

## 6. Golden fixtures

[`golden/outbox-journal-v1-manifest.json`](golden/outbox-journal-v1-manifest.json)이
fixture 길이와 SHA-256을 고정한다.

- `outbox-journal-valid.hex`: Intent 2개와 valid BatchCommit
- `outbox-journal-partial.hex`: 같은 batch의 잘린 BatchCommit tail
- `outbox-journal-corrupt.hex`: valid batch 뒤 CRC가 손상된 Retired tail
- `outbox-journal-retired.hex`: valid batch와 valid Retired

`.hex` 파일은 ASCII whitespace를 무시하고 decode한 bytes가 계약 대상이다.
