from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock


try:
    from tools.load.loot_load.evidence.fact_writer import JsonlFactWriter, write_json_atomic
except ModuleNotFoundError:
    JsonlFactWriter = None
    write_json_atomic = None


class FactWriterTests(unittest.TestCase):
    def setUp(self) -> None:
        if JsonlFactWriter is None:
            self.fail("immutable fact writer module is absent")

    def test_jsonl_records_have_monotonic_sequence_and_flush_fsyncs(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "raw" / "worker-0.jsonl"
            with mock.patch("os.fsync", wraps=__import__("os").fsync) as fsync:
                with JsonlFactWriter(path, stream_id="worker-0") as writer:
                    writer.append({"type": "READY"})
                    writer.append({"type": "HEARTBEAT"})
                    writer.flush()
            records = [json.loads(line) for line in path.read_text().splitlines()]
            self.assertEqual([1, 2], [record["sequence"] for record in records])
            self.assertEqual(["worker-0", "worker-0"], [record["streamId"] for record in records])
            self.assertGreaterEqual(fsync.call_count, 1)

    def test_existing_fact_stream_is_never_overwritten_or_reopened(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "facts.jsonl"
            with JsonlFactWriter(path, stream_id="parent") as writer:
                writer.append({"type": "CREATED"})
            before = path.read_bytes()
            with self.assertRaises(FileExistsError):
                JsonlFactWriter(path, stream_id="parent")
            self.assertEqual(before, path.read_bytes())

    def test_manifest_write_uses_same_directory_replace_and_leaves_no_temp_file(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manifest" / "run-input.json"
            with mock.patch("os.replace", wraps=__import__("os").replace) as replace:
                write_json_atomic(path, {"b": 2, "a": 1})
            self.assertEqual('{"a":1,"b":2}\n', path.read_text())
            self.assertEqual(path.parent, Path(replace.call_args.args[0]).parent)
            self.assertEqual([], list(path.parent.glob("*.tmp")))


if __name__ == "__main__":
    unittest.main()
