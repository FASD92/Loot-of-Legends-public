#!/usr/bin/env python3
"""Verify public portfolio claims against the tracked repository tree."""

from __future__ import annotations

import argparse
import posixpath
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath
from urllib.parse import unquote, urlsplit


EVIDENCE_PATHS = (
    "CMakeLists.txt",
    "README.md",
    "docs/recruiter-audit.md",
    "docs/verification.md",
    "game-server/platform/transport-tcp/include/lol/transport/tcp/TcpConnection.hpp",
    "game-server/platform/runtime-linux/include/lol/runtime/linux/EpollReactor.hpp",
    "game-server/platform/transport-rudp/include/lol/transport/rudp/ReliableQueue.hpp",
    "game-server/modules/game-flow/src/execution/RoomExecutionCell.hpp",
    "game-server/modules/battle/include/lol/battle/BattleLoadApi.hpp",
    "tests/transport-rudp/RudpDeliveryTests.cpp",
    "tests/integration/rudp-bind/RudpBindTests.cpp",
    "tools/load/loot_load/runner/runtime.py",
    "tools/load/tests/test_runner_runtime.py",
    "meta-server/src/main/java/com/fasd92/lootoflegends/meta/platform/mysql/JdbcSettlementInbox.java",
    "meta-server/src/test/java/com/fasd92/lootoflegends/meta/settlement/SettlementAcceptanceTest.java",
    "client/unity/Assets/LootOfLegends/Tests/EditMode/RudpCombatClientTests.cs",
)

FORBIDDEN_PATH = re.compile(
    r"^(?:START_HERE|AUTHORITY|CURRENT|HANDOFF|CHANGE_CONTROL)\.md$"
    r"|^(?:docs/(?:foundation|plans|specs|evidence|reviews|runbooks|superpowers)|infra|deploy|artifacts?)(?:/|$)"
    r"|^contracts/(?:deploy|program)(?:/|$)"
    r"|^contracts/meta-api/private-evidence-admission-v1\.schema\.json$"
    r"|^contracts/evidence/(?!reason-codes\.v1\.json$)"
    r"|^scripts/(?:evidence|release)(?:/|$)"
    r"|(?:^|/)(?:restricted|raw)(?:/|$)"
    r"|(?:^|/)(?:\.env(?:\..+)?|[^/]+\.(?:pem|key|p12|pfx|jks|keystore|zip|tar|tgz|7z|pcap|har))$",
    re.IGNORECASE,
)
MARKDOWN_LINK = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")
BACKTICK = re.compile(r"(?<!`)`([^`\n]+)`(?!`)")
TEST_COUNT_MARKER = re.compile(
    r"<!--\s*portfolio-test-count:\s*ctest=(\d+)\s+load-unittest=(\d+)\s*-->"
)
PRIVATE_KEY = re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----")
CREDENTIAL_URL = re.compile(
    r"https?://[^\s/:@\"']+:[^\s/@\"']+@[^\s\"']+"
)
HIGH_CONFIDENCE_TOKEN = re.compile(
    r"(?:AKIA[0-9A-Z]{16}|ASIA[0-9A-Z]{16}|github_pat_[A-Za-z0-9_]{20,}"
    r"|gh[pousr]_[A-Za-z0-9_]{20,}|xox[baprs]-[A-Za-z0-9-]{20,}"
    r"|AIza[0-9A-Za-z_-]{20,}|arn:"
    r"aws:[^\s\"']+|\b\d{12}\.dkr\.ecr\b)"
)
ABSOLUTE_USER_PATH = re.compile(
    r"(?:^|[\"'\s])(?:/Users/[^/\s\"']+|/home/[^/\s\"']+|[A-Za-z]:\\Users\\[^\\\s\"']+)(?:[/\\][^\s\"']*)?",
    re.MULTILINE,
)
INTERNAL_HOST = re.compile(r"\b[A-Za-z0-9.-]+\.(?:slice9|release)\.internal\b")
PATH_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cs",
    ".h",
    ".hpp",
    ".java",
    ".json",
    ".md",
    ".py",
    ".sh",
    ".yml",
    ".yaml",
}


def tracked_files(repo: Path) -> set[str]:
    result = subprocess.run(
        ["git", "ls-files", "-z"],
        cwd=repo,
        capture_output=True,
        check=True,
    )
    return {entry.decode("utf-8") for entry in result.stdout.split(b"\0") if entry}


def read_tracked(repo: Path, relative_path: str) -> str:
    return (repo / relative_path).read_text(encoding="utf-8", errors="replace")


def read_tracked_text(repo: Path, relative_path: str) -> str | None:
    content = (repo / relative_path).read_bytes()
    if b"\0" in content:
        return None
    return content.decode("utf-8", errors="replace")


def normalize_link(markdown_path: str, target: str) -> str | None:
    target = target.strip().split(maxsplit=1)[0].strip("<>")
    parsed = urlsplit(target)
    if parsed.scheme or parsed.netloc or target.startswith(("#", "mailto:")):
        return None
    path = unquote(parsed.path)
    if not path:
        return None
    if path.startswith("/"):
        return path.removeprefix("/")
    return posixpath.normpath(str(PurePosixPath(markdown_path).parent / path))


def looks_like_repo_path(value: str) -> bool:
    value = value.strip().strip(".,:;()[]{}<>")
    if " " in value or value.startswith(("http://", "https://", "/")):
        return False
    path = PurePosixPath(value.split("#", 1)[0])
    return len(path.parts) > 1 and path.suffix.lower() in PATH_SUFFIXES


def count_ctest_registrations(repo: Path, tracked: set[str]) -> int:
    pattern = re.compile(r"(?<![A-Za-z0-9_])add_test\s*\(", re.IGNORECASE)
    return sum(
        len(pattern.findall(read_tracked(repo, path)))
        for path in tracked
        if PurePosixPath(path).name == "CMakeLists.txt"
    )


def count_load_unittests(repo: Path, tracked: set[str]) -> int:
    pattern = re.compile(r"^\s+def\s+test_[A-Za-z0-9_]+\s*\(", re.MULTILINE)
    return sum(
        len(pattern.findall(read_tracked(repo, path)))
        for path in tracked
        if path.startswith("tools/load/tests/test_") and path.endswith(".py")
    )


def verify(repo: Path) -> list[str]:
    tracked = tracked_files(repo)
    failures: list[str] = []

    for path in EVIDENCE_PATHS:
        if path not in tracked:
            failures.append(f"missing required evidence path: {path}")

    for path in sorted(tracked):
        if FORBIDDEN_PATH.search(path):
            failures.append(f"forbidden public path: {path}")

    markdown_paths = sorted(path for path in tracked if path.lower().endswith(".md"))
    for markdown_path in markdown_paths:
        content = read_tracked(repo, markdown_path)
        for match in MARKDOWN_LINK.finditer(content):
            target = normalize_link(markdown_path, match.group(1))
            if target is not None and target not in tracked:
                failures.append(f"broken local Markdown link in {markdown_path}: {target}")
        for value in BACKTICK.findall(content):
            candidate = value.strip().strip(".,:;()[]{}<>")
            candidate_path = candidate.split("#", 1)[0]
            resolved = str(PurePosixPath(markdown_path).parent / candidate_path)
            if looks_like_repo_path(candidate) and resolved not in tracked and candidate_path not in tracked:
                failures.append(f"missing backtick path in {markdown_path}: {candidate}")

    if "README.md" in tracked:
        readme = read_tracked(repo, "README.md")
        marker = TEST_COUNT_MARKER.search(readme)
        if marker is None:
            failures.append("README is missing the portfolio test-count marker")
        else:
            claimed_ctest = int(marker.group(1))
            claimed_load = int(marker.group(2))
            actual_ctest = count_ctest_registrations(repo, tracked)
            actual_load = count_load_unittests(repo, tracked)
            if claimed_ctest != actual_ctest:
                failures.append(
                    "test-count mismatch: README claims "
                    f"ctest={claimed_ctest}, tracked tree has {actual_ctest} add_test registrations"
                )
            if claimed_load != actual_load:
                failures.append(
                    "test-count mismatch: README claims "
                    f"load-unittest={claimed_load}, tracked tree has {actual_load} test methods"
                )

    for path in sorted(tracked):
        content = read_tracked_text(repo, path)
        if content is None:
            continue
        if PRIVATE_KEY.search(content):
            failures.append(f"private key material in tracked file: {path}")
        if CREDENTIAL_URL.search(content):
            failures.append(f"credential-bearing URL in tracked file: {path}")
        if HIGH_CONFIDENCE_TOKEN.search(content):
            failures.append(f"credential-like token in tracked file: {path}")
        if ABSOLUTE_USER_PATH.search(content):
            failures.append(f"absolute user path in tracked file: {path}")
        if INTERNAL_HOST.search(content):
            failures.append(f"private internal hostname in tracked file: {path}")

    return failures


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args(argv)
    repo = args.repo.resolve()

    try:
        failures = verify(repo)
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"FAIL: unable to inspect tracked tree: {error}")
        print("Boundary: static claim verification; tests were not executed.")
        return 1

    if failures:
        print("FAIL: public portfolio claim verification")
        for failure in failures:
            print(f"- {failure}")
        print("Boundary: static claim verification; tests were not executed.")
        return 1

    print("PASS: public portfolio claims match the tracked repository tree")
    print("Boundary: static claim verification; tests were not executed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
