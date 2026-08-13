from __future__ import annotations

import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
GOLDEN = ROOT / "contracts" / "protocol" / "golden"

try:
    from tools.load.loot_load.protocol.rudp import (
        AckTracker,
        ProtocolError,
        ReliableSendQueue,
        decode_rudp_datagram,
        encode_rudp_message,
    )
    from tools.load.loot_load.protocol.tcp import decode_tcp_frame, encode_tcp_message
except ModuleNotFoundError:
    AckTracker = None
    ProtocolError = None
    ReliableSendQueue = None
    decode_rudp_datagram = None
    encode_rudp_message = None
    decode_tcp_frame = None
    encode_tcp_message = None


class ProtocolContractTests(unittest.TestCase):
    def setUp(self) -> None:
        if decode_tcp_frame is None:
            self.fail("protocol codec modules are absent")

    def test_every_tcp_golden_vector_round_trips(self):
        count = 0
        for path in GOLDEN.glob("*.json"):
            contract = json.loads(path.read_text())
            for vector in contract.get("tcpGoldenVectors", []):
                frame = bytes.fromhex(vector["frameHex"])
                message = decode_tcp_frame(frame)
                self.assertEqual(frame, encode_tcp_message(message.name, message.fields), path.name)
                count += 1
        self.assertEqual(27, count)

    def test_every_rudp_golden_vector_round_trips(self):
        count = 0
        for path in GOLDEN.glob("*.json"):
            contract = json.loads(path.read_text())
            for vector in contract.get("rudpGoldenVectors", []):
                datagram = bytes.fromhex(vector["datagramHex"])
                message = decode_rudp_datagram(datagram)
                self.assertEqual(
                    datagram,
                    encode_rudp_message(message.name, message.header, message.fields),
                    f"{path.name}:{vector['semanticName']}",
                )
                count += 1
        self.assertEqual(14, count)

    def test_tcp_negative_corpus_and_unknown_message_are_rejected(self):
        contract = json.loads((GOLDEN / "battle-recovery-v1.json").read_text())
        for vector in contract["tcpNegativeCorpus"]:
            with self.assertRaises(ProtocolError, msg=vector["semanticName"]):
                decode_tcp_frame(bytes.fromhex(vector["frameHex"]))
        valid = bytearray.fromhex(
            json.loads((GOLDEN / "session-auth-v1.json").read_text())["tcpGoldenVectors"][0]["frameHex"]
        )
        valid[5:9] = (999).to_bytes(4, "big")
        with self.assertRaises(ProtocolError):
            decode_tcp_frame(bytes(valid))

    def test_rudp_checksum_and_unknown_message_are_rejected(self):
        contract = json.loads((GOLDEN / "rudp-bind-v1.json").read_text())
        datagram = bytearray.fromhex(contract["rudpGoldenVectors"][0]["datagramHex"])
        datagram[-1] ^= 1
        with self.assertRaises(ProtocolError):
            decode_rudp_datagram(bytes(datagram))
        decoded = decode_rudp_datagram(
            bytes.fromhex(contract["rudpGoldenVectors"][0]["datagramHex"])
        )
        header = dict(decoded.header)
        header["messageId"] = 999
        with self.assertRaises(ProtocolError):
            encode_rudp_message("Unknown", header, {})

    def test_ack_tracker_matches_ack_bits_contract(self):
        tracker = AckTracker()
        self.assertEqual("NEWEST", tracker.observe(5))
        self.assertEqual("REORDERED", tracker.observe(3))
        self.assertEqual("DUPLICATE", tracker.observe(3))
        self.assertEqual((5, 0b10), tracker.state)
        self.assertTrue(tracker.is_acknowledged(3, 5, 0b10))
        self.assertFalse(tracker.is_acknowledged(4, 5, 0b10))

    def test_reliable_queue_separates_transmit_retry_ack_and_timeout(self):
        queue = ReliableSendQueue()
        queue.enqueue(1, b"one", lane="APPLICATION", now=0.0)
        first = queue.poll(now=0.0)
        retry = queue.poll(now=0.2)
        self.assertEqual((1, 1, "TRANSMIT"), (first[0].sequence, first[0].attempt, first[0].kind))
        self.assertEqual((1, 2, "RETRY"), (retry[0].sequence, retry[0].attempt, retry[0].kind))
        self.assertEqual([1], queue.acknowledge(ack=1, ack_bits=0))

        queue.enqueue(2, b"two", lane="APPLICATION", now=1.0)
        self.assertEqual([2], queue.expire(now=6.0))


if __name__ == "__main__":
    unittest.main()
