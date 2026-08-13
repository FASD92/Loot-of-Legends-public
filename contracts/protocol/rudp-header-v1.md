# Custom RUDP v1 Header Contract

Status: `Accepted 1.0.0`

모든 integer는 network byte order이고 header 크기는 정확히 48 bytes다.

| Offset | Field | Type / bytes | Contract |
| ---: | --- | --- | --- |
| 0 | `magic` | `u32 / 4` | `0x4C4F4C32` (`LOL2`) |
| 4 | `protocolMajor` | `u8 / 1` | `1` |
| 5 | `flags` | `u8 / 1` | 아래 exact flag 중 하나 |
| 6 | `headerBytes` | `u16 / 2` | `48` |
| 8 | `sessionId` | `u64 / 8` | nonzero authenticated Session |
| 16 | `sessionGeneration` | `u64 / 8` | nonzero current generation |
| 24 | `transportEpoch` | `u32 / 4` | BindHello는 `0`, bind 뒤에는 nonzero |
| 28 | `sequence` | `u32 / 4` | nonzero, wrap 뒤 `1`부터 재개 |
| 32 | `ack` | `u32 / 4` | newest received sequence, history가 없으면 `0` |
| 36 | `ackBits` | `u32 / 4` | bit 0은 `ack-1`, bit 31은 `ack-32` |
| 40 | `messageId` | `u16 / 2` | public registry ID, ACK-only는 `0` |
| 42 | `payloadBytes` | `u16 / 2` | datagram의 실제 remaining bytes |
| 44 | `crc32` | `u32 / 4` | 아래 CRC32 결과 |

## Flags

| Value | Meaning | Additional contract |
| ---: | --- | --- |
| `0x00` | unreliable application/control | registry message와 payload 사용 |
| `0x01` | reliable | bounded reliable queue와 ACK 대상 |
| `0x02` | ACK-only | `messageId=0`, `payloadBytes=0` |
| `0x04` | heartbeat | `RudpHeartbeat`, empty payload |

flag 조합과 `0xF8` reserved bit는 v1에서 모두 reject한다.

## CRC32

- algorithm: CRC-32/ISO-HDLC (`poly=0x04C11DB7`, reflected
  `0xEDB88320`, init/final xor `0xFFFFFFFF`)
- input: `crc32` field를 zero로 둔 48-byte header 뒤에 payload를 연결한 bytes
- output: unsigned 32-bit 결과를 header에 network byte order로 기록
- CRC, magic, version, header length, payload length, flag 검증은 message decode와
  peer/domain mutation보다 먼저 수행한다.

## Datagram과 sequence

- header를 포함한 datagram maximum은 `1200` bytes다.
- v1 application fragmentation/reassembly는 없다.
- serial 비교는 `a != b && uint32(a - b) < 0x80000000`이면 `a`가 `b`보다
  newer인 32-bit half-range arithmetic을 사용한다.
- 송신 sequence는 `1`부터 시작하고 `UINT32_MAX` 다음 `1`로 돌아간다.
- ACK history는 newest `ack`와 이전 32개 receipt bitmap이다. ACK는 datagram
  수신 사실만 뜻하며 application command 결과가 아니다.
