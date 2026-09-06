# Battle Continuity Journal and Checkpoint Contract v1

Status: Accepted

Contract version: 1.0.4

Contract ID: `LOL-V2-BATTLE-CONTINUITY-V1`

Canonical semantic owner: `lol_battle_continuity`

Canonical storage adapter: `lol_battle_continuity_storage`

## 1. 목적과 보장 경계

이 계약은 같은 host와 같은 endpoint에서 GameServer process를 다시 시작할 때 진행 중인
Battle을 마지막 durable checkpoint와 이후 authoritative command journal로 복구하는
binary 형식, logical time, identity, durability, failure isolation을 고정한다.

보장 범위는 다음과 같다.

- `TickCommit`이 `fdatasync`된 뒤 외부에 성공 또는 그 상태를 반영한 snapshot을 보낸
  command는 process crash 뒤 유실하지 않는다.
- 아직 성공을 관측하지 못한 command의 RPO는 한 logical tick, 즉 최대 `50ms`다.
- 이 RPO는 같은 host의 local durable filesystem, 정상적인 `fdatasync` 의미, 단일
  GameServer writer라는 전제에 한정한다. storage media 손상, host 유실, 다른 node 복구는
  zero-loss 주장에 포함하지 않는다.
- process downtime 동안 Battle logical time은 흐르지 않는다.

Raw TCP/RUDP packet, credential, token, AccountId, nickname, IP, endpoint, OS timer 또는 C++
object memory는 journal 대상이 아니다.

## 2. Stable identity와 recovery epoch

기존 allocation은 settlement sequence에서 새 `RoomId`를 추론하므로 settlement 전에
process가 죽으면 같은 `RoomId + BattleInstanceId`가 다시 발급될 수 있다. v1부터 새
Room의 `RoomId`는 다음 unsigned 64-bit 값이다.

```text
RoomId = (originRecoveryEpoch << 32) | roomOrdinal
originRecoveryEpoch: u32, nonzero
roomOrdinal: u32, nonzero, 해당 epoch에서 단조 증가
```

따라서 public Battle identity는 기존 `(RoomId, BattleInstanceId)`를 그대로 유지하면서
process restart 뒤에도 충돌하지 않는다. `originRecoveryEpoch`은 `RoomId`에서 추출할 수
있으며 record header에서 일치 여부를 다시 검증한다. 이것은 client authority가 아니다.
Client는 기존 fresh Game credential이 증명한 Account와 복구된 participant mapping으로만
같은 Battle에 복귀한다.

Continuity root는 process lifetime 동안 exclusive lock fd를 소유한다. Lock을 얻은
process만 atomic manifest의 `writerRecoveryEpoch`을 1 증가시킬 수 있다. 새 Room은 현재
writer epoch를 origin epoch로 고정한다. 복구된 Battle은 기존 origin epoch/RoomId/BattleId를
보존하고 새 writer epoch로만 append한다. Storage write request, record, completion에는 모두
expected writer epoch가 있으며 현재 lease와 다르면 no-write reject한다.

복구된 participant의 `SessionId`는 보존한다. `SessionGeneration`, TCP connection epoch,
RUDP capability와 `TransportEpoch`은 새 process에서 다시 발급한다. 이전 process의 packet과
generation은 mutation 권위를 갖지 않는다.

## 3. Logical Battle time

- `logicalTick`: `u64`, Battle 시작은 `0`, `20Hz` 고정
- `tickNanos`: `50,000,000`
- `battleElapsedNanos`: `u64`, `logicalTick * tickNanos`
- cooldown, rate-limit, deadline, retained-result expiry 중 Battle 결과에 영향을 주는 값은
  logical tick 또는 battle-relative nanoseconds로만 저장한다.
- live process의 scheduling에는 `steady_clock`을 사용할 수 있지만 durable state와 replay
  입력에는 넣지 않는다.
- process downtime은 logical time을 freeze한다. Recovery가 `READY`가 된 뒤 다음 tick부터
  진행한다.
- 기존 30초 reconnect grace는 transport lifecycle timer다. Recovery 뒤 모든 restored
  participant는 새 `READY + 30초` grace를 받고, 이 timer는 canonical state hash에 넣지 않는다.

## 4. Canonical command boundary와 순서

Journal source는 transport decode, authentication, current generation/epoch, request shape,
transport duplicate/reorder 검사를 통과한 뒤 Room mailbox가 linearize하고 Battle domain
admission이 실제 mutation에 사용한 canonical command다.

Canonical command identity는 다음으로 구성한다. Attack/Loot의 기존 128-bit CommandId는
high/low를 lossless하게 보존하고 Movement ActionSequence는 high=0, low=u32 값으로 올린다.

```text
ParticipantSlot u16
CommandKind u16
CommandIdHigh u64
CommandIdLow u64
normalized payload (command별 fixed integer fields)
```

AccountId, nickname, socket, packet sequence, SessionGeneration과 client timestamp는 포함하지
않는다. Raw duplicate, stale generation, invalid payload, authentication failure는 record를
만들지 않는다. Domain에 도달한 새 command가 rate-limit timestamp, cooldown, dedup/result
store, loot ownership 또는 다른 replay state를 변경하면 success/reject 여부와 관계없이
`CommandDecision`을 만든다. 같은 CommandId의 pure replay처럼 아무 state도 바꾸지 않는
요청은 새 record를 만들지 않는다.

ArenaLoadComplete, MovementTick, load/combat/loot deadline, participant exit, input
suspend/resume처럼 client CommandId가 없는 state-changing command도 같은 경계에서 기록한다.
이때 server command의 ParticipantSlot은 예약값 `0`, CommandIdHigh는 `0`, CommandIdLow는
해당 CommandDecision의 recordSequence다. Participant를 대상으로 하는 system command의
대상 slot은 payload에 넣는다. System command가 state를 바꾸지 않았다면 participant
command와 동일하게 record를 만들지 않는다.

같은 logical tick에서는 Room mailbox admission order가 command order다. Record sequence가
그 순서를 고정하며 replay는 다른 정렬을 적용하지 않는다.

## 5. Ruleset, RNG와 canonical state

`BattleRulesetVersion=1`은 다음 묶음을 뜻한다.

- combat ruleset `4`
- relic/drop catalog ruleset `1`
- SplitMix64 drop RNG `1`
- integer participant spawn table `1`
- canonical state encoding `1`

이 composite version은 위 이름만이 아니라 production `BattleInstance` mutation에 도달하는
movement bounds/speed/rate, combat damage/range/rate/cooldown, load/combat/loot deadline,
command-result retention, drop/catalog, loot claim range/resolution, terminal ranking과 모든
integer constant/algorithm을 함께 고정한다. 이 중 하나라도 replay 결과를 바꾸면 기존 version을
조용히 재사용하지 않고 `BattleRulesetVersion`을 올린다.

Battle 시작 때 explicit nonzero `BattleSeed u64`를 고정하고 모든 RNG state를 checkpoint에
포함한다. Replay는 알 수 없는 ruleset version을 명시적으로 거부한다.

Canonical bytes는 unsigned/signed fixed-width integer와 length-prefixed bytes를 big-endian으로
인코딩한다. Boolean은 `u8 0/1`, enum은 계약된 unsigned 값이다. Pointer, padding, native
endian, floating point, locale, wall clock은 금지한다. Participant/monster/drop/dedup/result
collection은 stable ID 오름차순으로 정렬한다. Spawn position은 정수 lookup table만 사용한다.

State hash는 canonical state bytes 전체의 SHA-256이다. 최소 포함 상태는 다음과 같다.

- stable Battle identity, ruleset version, seed, logical tick/elapsed time, phase/deadline
- ParticipantSlot/SessionId, position, gameplay status, score와 captured membership
- monster state, RNG state, drop state와 owner ParticipantSlot
- cooldown/rate/dedup/result-store의 logical timestamp, command identity와 retained outcome
- terminal result, ranking, loot ownership, settlement batch/intent identity와 payload hash
- 복구에 필요한 최소 Room shell, host slot, member slots, Room phase와 next battle ordinal

Socket/endpoint/current writer epoch/transport generation/reconnect grace는 hash에서 제외한다.

## 6. Record envelope

각 active Battle은 하나의 append-only journal file을 가진다. 모든 정수는 big-endian이다.
`recordLength == 68 + payloadLength`여야 한다.

| Offset | Size | Field | Contract |
|---:|---:|---|---|
| 0 | 4 | magic | `0x4C424331` (`LBC1`) |
| 4 | 2 | schemaVersion | `1` |
| 6 | 2 | recordType | 아래 값 |
| 8 | 4 | recordLength | envelope + payload + CRC 전체 |
| 12 | 8 | recordSequence | 1부터 엄격히 증가 |
| 20 | 4 | originRecoveryEpoch | `RoomId >> 32`와 동일 |
| 24 | 4 | writerRecoveryEpoch | current lease와 동일 |
| 28 | 8 | roomId | nonzero stable RoomId |
| 36 | 8 | battleInstanceId | nonzero stable BattleInstanceId |
| 44 | 8 | logicalTick | nondecreasing |
| 52 | 8 | battleElapsedNanos | `logicalTick * 50,000,000` |
| 60 | 4 | payloadLength | record별 payload bytes |
| 64 | N | payload | canonical record payload |
| 64+N | 4 | crc32 | `bytes[0..64+N)` IEEE CRC-32 |

CRC-32는 reflected polynomial `0xEDB88320`, initial/final XOR `0xFFFFFFFF`다. Parser는
length를 신뢰하기 전에 file remaining bytes와 `1MiB` record limit를 확인한다. 한 Battle
journal의 v1 hard limit는 `64MiB`이며 초과 시 해당 Battle은 fail-closed recovery failure로
전환하고 성공을 보내지 않는다.

Decoder의 `committedBytes`는 마지막으로 정상 검증된 `TickCommit` record 끝의 byte offset이다.
부분 기록, checksum 오류 또는 아직 commit되지 않은 batch는 `records`와 이 offset에 포함하지
않는다. 첫 committed batch는 다음 세 record를 정확히 포함해야 한다.

```text
seq=1  BattleStart  logicalTick=0
seq=2  Checkpoint   logicalTick=0  stateHash == BattleStart.initialStateHash
seq=3  TickCommit   logicalTick=0
```

## 7. Record payload

### 7.1 BattleStart (`recordType=1`)

```text
BattleRulesetVersion u32
BattleSeed u64
tickHertz u32 (=20)
initialStateHash[32]
participantCount u16 (2..10)
repeat ParticipantSlot order:
  ParticipantSlot u16
  SessionId u64
```

암호화 participant envelope가 먼저 durable해지고 BattleStart와 initial Checkpoint가 한
tick batch로 commit된 뒤에만 Battle entry를 외부에 보인다.

### 7.2 CommandDecision (`recordType=2`)

```text
ParticipantSlot u16
CommandKind u16
CommandIdHigh u64
CommandIdLow u64
decisionCode u16
commandPayloadLength u32
commandPayload[commandPayloadLength]
outcomePayloadLength u32
outcomePayload[outcomePayloadLength]
roomRecoveryStateLength u32
roomRecoveryState[roomRecoveryStateLength]
postDecisionStateHash[32]
```

Command/outcome payload는 ruleset이 정의한 fixed integer canonical bytes다. Replay는 각
record 직후 hash를 비교하므로 변조나 order 변경의 최초 divergent record sequence/tick을
출력할 수 있다.

`roomRecoveryState`는 credential과 개인정보를 제외한 length-prefixed canonical minimal
Room projection이다. Checkpoint 사이의 Room lifecycle, host/member slot, ready 상태와 next
Battle ordinal을 같은 decision 뒤 상태로 복원하며 별도 authority나 event stream이 아니다.

### 7.3 Checkpoint (`recordType=3`)

```text
checkpointSchemaVersion u16 (=1)
stateBytesLength u32
canonicalStateBytes[stateBytesLength]
stateHash[32]
```

Checkpoint는 Battle start, 매 20번째 durable tick, phase transition, terminal에 만든다.
Restore는 마지막 valid committed Checkpoint의 full state를 import하고 이후
CommandDecision을 production Battle rule로 replay한다.

### 7.4 TerminalReceipt (`recordType=4`)

```text
terminalReason u16
finalStateHash[32]
resultPayloadLength u32
canonicalResultPayload[resultPayloadLength]
resultCommittedUnixEpochMilliseconds u64
resultCommittedBattleElapsedNanos u64
SettlementBatchId[16]
settlementIntentCount u16
repeat canonical ParticipantSlot order:
  ParticipantSlot u16
  SettlementId[16]
  settlementPayloadHash[32]
```

Receipt의 settlement identity와 payload hash는 기존 settlement 계약이 생성한 값을 그대로
보존하며 recovery가 새 identity를 만들지 않는다. Terminal decision에서 wall clock을 한 번만
읽어 `resultCommittedUnixEpochMilliseconds` 결정으로 기록하고, 기존
`ResultCommittedAt.monotonicNanoseconds` field에는 process-local steady epoch 대신 receipt의
`resultCommittedBattleElapsedNanos`를 넣는다. 이 두 값은 기존 settlement canonical payload
bytes를 동일하게 재생성하는 입력이며 simulation과 deadline 판단에는 사용하지 않는다.

Terminal hash는 다음 canonical big-endian bytes를 순서대로 해시한 SHA-256이다.
`finalStateHash`는 해시 결과를 담는 출력이므로 입력에서 제외한다.

```text
canonicalBattleStateBytes
terminalFields
canonicalResultPayload
repeat unique ParticipantSlot ascending:
  ParticipantSlot u16
  SettlementId[16]
  settlementPayloadHash[32]
```

### 7.5 TickCommit (`recordType=5`)

```text
firstRecordSequence u64
lastDataRecordSequence u64
recordCount u32
committedStateHash[32]
```

한 batch에서 TickCommit은 마지막 record다. Writer는 batch 전체를 append한 뒤 data sync를
한 번 수행한다. Sync 성공 completion 전에는 다음 tick mutation과 authoritative output을
열지 않는다. Battle마다 pending durable tick은 최대 하나다.

## 8. Checkpoint와 recovery

Startup 순서는 다음과 같다.

```text
exclusive root lock
  -> key/root/manifest 검증
  -> writerRecoveryEpoch atomic increment
  -> RECOVERING
  -> active Battle journal별 scan
  -> 같은 Room은 가장 큰 BattleInstanceId 하나만 install 후보로 선택
  -> last committed Checkpoint import
  -> later CommandDecision production replay
  -> every recorded hash 검증
  -> healthy Battle + minimal Room/session shell install
  -> listener bind
  -> READY
```

- EOF가 record 중간이거나 last valid TickCommit 뒤의 record length/CRC가 잘못되면 그 tail은
  unique quarantine file에 보존하고 source는 last valid TickCommit offset으로 truncate/data
  sync한다. 같은 crash tail 복구를 반복해도 기존 quarantine 이름 충돌로 startup을 막지 않는다.
- Invalid record 뒤에 valid later TickCommit가 존재하거나 committed checkpoint import/replay
  hash가 다르면 committed corruption이다. 해당 Battle 하나만 quarantine하고 열지 않는다.
- 하나의 Battle quarantine은 다른 healthy Battle의 restore와 server readiness를 막지 않는다.
  그 Battle의 reconnect는 기존 `AuthenticationRejected(RESUME_UNAVAILABLE)`로 끝난다.
- 같은 `RoomId`에 여러 healthy journal이 있으면 filesystem 순서를 사용하지 않는다. Session과
  Room을 설치하기 전에 unsigned `BattleInstanceId`가 가장 큰 journal 하나만 복구 후보로
  선택한다. 더 작은 journal에 committed `TerminalReceipt`가 있으면 successor가 존재한다는
  뜻이므로 durable retire tombstone을 기록한다. 더 작은 journal이 nonterminal이면 불가능한
  predecessor 충돌로 quarantine한다. 이 중재가 실패하면 listener를 열지 않는다.
- root lock, root directory, epoch manifest 또는 key validation 실패는 전체 startup을
  fail-closed한다.

## 9. Participant privacy envelope

ParticipantSlot과 AccountId/nickname, Room title 및 Battle 시작 시점의 original
SessionGeneration 복구 mapping은 journal과 분리한 AES-256-GCM sidecar에 저장한다.
Original generation은 Battle 수명 동안 보존하되, 각 recovery writer epoch에서 fresh
Game credential과 결합한 resume lookup에 한 번만 사용하고 runtime proof에서
폐기한다. 이 값은 gameplay/RUDP authority가 아니다. Startup은 aggregate/route에 사용할
fresh generation을 별도로 발급하고, resume snapshot은 그 fresh generation과 일치해야
한다. 클라이언트는 current generation이 `RESUME_UNAVAILABLE`로 거부된 경우 fresh
credential과 새 TCP connection으로 original proof를 단 한 번 fallback할 수 있다.

- key는 operator가 제공한 정확히 32-byte local file이며 regular file, owner read/write만
  허용한다. symlink, group/other permission, short/long key는 startup failure다.
- sidecar는 schema/version, stable Battle identity, 96-bit random nonce, ciphertext와 128-bit
  tag를 가진다. Stable identity와 schema는 AAD다.
- sidecar는 same-directory temporary file data sync, atomic rename, parent directory sync로
  게시한다.
- raw key, plaintext identity, credential과 token은 log/evidence/journal에 남기지 않는다.
- v1은 key rotation, KMS와 다른 host decrypt를 지원하지 않는다.

## 10. Settlement와 visibility

Terminal 순서는 고정이다.

```text
TerminalReceipt + terminal Checkpoint + TickCommit durable
  -> existing SettlementIntentBatch를 기존 SegmentJournal에 durable append
  -> Result visible / Room OPEN
```

첫 단계 뒤 crash하면 recovery가 receipt의 같은 `SettlementBatchId/SettlementId/payloadHash`로
기존 settlement path를 다시 진행한다. 두 번째 단계 뒤 crash하면 기존 outbox/inbox
idempotency와 Meta unique identity가 중복 자산 적용을 막는다. Loot owner와 immutable Result는
continuity state에서 복원하며 새로 판정하지 않는다.

## 11. Public reconnect와 readiness

새 public message ID는 없다. `LOL-V2-BATTLE-RESUME-V1`의 fresh credential,
`BattleResumeSnapshot`, `BattleResumeSnapshotApplied` barrier를 그대로 사용한다. Restore된
session은 fresh generation/capability/TransportEpoch를 발급하고 snapshot ACK 전 gameplay
input을 차단한다. Recovery가 끝나기 전 listener를 열거나 `READY`를 발표하지 않는다.

## 12. 명시적 비목표

Kubernetes/ECS/GameLift, service discovery, 다른 host migration, multi-region consensus,
sharding, 범용 event-sourcing/repository, 전체 Lobby/Room persistence, compaction, KMS/key
rotation과 settlement journal format 재사용은 v1 범위가 아니다.
