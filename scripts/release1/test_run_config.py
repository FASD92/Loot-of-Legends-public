import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from scripts.release1.capacity_report import (
    load_json,
    sha256_file,
    validate_schema,
)
from scripts.release1.run_config import (
    build_execution_target,
    build_run_config,
    main,
)


REPO_ROOT = Path(__file__).resolve().parents[2]
GATE_CONFIG_PATH = REPO_ROOT / "scripts" / "release1" / "gate_config.json"
RUN_CONFIG_SCHEMA_PATH = (
    REPO_ROOT / "scripts" / "release1" / "schemas" / "run_config.schema.json"
)


class RunConfigProvenanceTests(unittest.TestCase):
    def test_execution_target_hashes_binary_and_filters_environment(self):
        with tempfile.TemporaryDirectory() as tmp:
            executable = Path(tmp) / "lol_server"
            executable.write_bytes(b"server-binary")

            target = build_execution_target(
                executable_path=executable,
                argv=["lol_server", "--metrics-textfile", "/tmp/lol_game.prom"],
                working_directory="/opt/lol",
                environment={
                    "LOL_ENV": "release1",
                    "PATH": "/usr/bin",
                    "SECRET_TOKEN": "raw-secret",
                },
                ulimit={"nofile_soft": 1024},
            )

        self.assertEqual(str(executable), target["executable_path"])
        self.assertEqual(
            hashlib.sha256(b"server-binary").hexdigest(),
            target["binary_sha256"],
        )
        self.assertEqual(
            ["lol_server", "--metrics-textfile", "/tmp/lol_game.prom"],
            target["argv"],
        )
        self.assertEqual("/opt/lol", target["working_directory"])
        self.assertEqual(
            {"LOL_ENV": "release1", "PATH": "/usr/bin"},
            target["environment"],
        )
        self.assertEqual({"nofile_soft": 1024}, target["ulimit"])

    def test_run_config_matches_existing_schema(self):
        with tempfile.TemporaryDirectory() as tmp:
            server = Path(tmp) / "lol_server"
            stress = Path(tmp) / "lol_epoll_stress"
            server.write_bytes(b"server")
            stress.write_bytes(b"stress")
            env = {"LOL_ENV": "release1", "SECRET_TOKEN": "raw"}
            ulimit = {"nofile_soft": 4096, "nofile_hard": 4096}

            run_config = build_run_config(
                run_id="20260701-010203-clean-deadbee",
                gate_config_path=GATE_CONFIG_PATH,
                server=build_execution_target(
                    server,
                    [str(server), "40000"],
                    tmp,
                    env,
                    ulimit,
                ),
                stress_runner=build_execution_target(
                    stress,
                    [str(stress)],
                    tmp,
                    env,
                    ulimit,
                ),
            )

        self.assertEqual(
            sha256_file(GATE_CONFIG_PATH),
            run_config["gate_config_sha256"],
        )
        validate_schema(run_config, load_json(RUN_CONFIG_SCHEMA_PATH))

    def test_cli_writes_schema_compatible_json_without_unapproved_env(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            server = root / "lol_server"
            stress = root / "lol_epoll_stress"
            output = root / "run_config.json"
            server.write_bytes(b"server")
            stress.write_bytes(b"stress")

            status = main(
                [
                    "--run-id",
                    "20260701-010203-clean-deadbee",
                    "--gate-config",
                    str(GATE_CONFIG_PATH),
                    "--server-executable",
                    str(server),
                    "--server-working-directory",
                    str(root),
                    "--server-argv-json",
                    json.dumps([str(server), "40000"]),
                    "--stress-runner-executable",
                    str(stress),
                    "--stress-runner-working-directory",
                    str(root),
                    "--stress-runner-argv-json",
                    json.dumps([str(stress)]),
                    "--output",
                    str(output),
                ],
                environment={
                    "LOL_ENV": "release1",
                    "SECRET_TOKEN": "raw-secret",
                },
                ulimit={"nofile_soft": 1024},
            )

            self.assertEqual(0, status)
            generated = load_json(output)

        validate_schema(generated, load_json(RUN_CONFIG_SCHEMA_PATH))
        self.assertEqual(
            {"LOL_ENV": "release1"},
            generated["execution"]["server"]["environment"],
        )
        self.assertNotIn("SECRET_TOKEN", json.dumps(generated))


if __name__ == "__main__":
    unittest.main()
