#!/usr/bin/env python3
"""Package Release 0 Unity Standalone client builds."""

from __future__ import annotations

import argparse
import json
import sys
import zipfile
from pathlib import Path
from urllib.parse import urlparse


WINDOWS_PACKAGE_NAME = "LootOfLegendsRelease0-Windows"
MACOS_PACKAGE_NAME = "LootOfLegendsRelease0-macOS"
WINDOWS_EXE_NAME = "LootOfLegendsRelease0.exe"
WINDOWS_DATA_DIR_NAME = "LootOfLegendsRelease0_Data"
MACOS_APP_NAME = "LootOfLegendsRelease0.app"
PACKAGE_README_NAME = "README-Release0.txt"
RELEASE0_CLIENT_CONFIG_NAME = "Release0ClientConfig.json"
WINDOWS_RELEASE0_CLIENT_CONFIG_PATH = (
    Path(WINDOWS_DATA_DIR_NAME) /
    "StreamingAssets" /
    RELEASE0_CLIENT_CONFIG_NAME
)
MACOS_RELEASE0_CLIENT_CONFIG_PATH = (
    Path(MACOS_APP_NAME) /
    "Contents" /
    "Resources" /
    "Data" /
    "StreamingAssets" /
    RELEASE0_CLIENT_CONFIG_NAME
)

PACKAGE_README = """Loot of Legends Release 0 Standalone Package

This package is public portfolio evidence only when the client signs in
through real Google OAuth, receives a Standalone Meta Session, passes Meta
Admission, authenticates to the C++ game server with a game session token, and
completes the lobby-room-arena-result-lobby flow without Unity Editor.

local dev-auth is not portfolio evidence.
Do not call this package smoke PASS if Unity Play Mode, manual copy-code,
embedded WebView, or a helper non-Standalone client was used.

Before sharing this package outside the local dev host, edit
Release0ClientConfig.json and set metaBaseUrl to the public Meta server URL.
"""


class PackageError(RuntimeError):
    """Raised when a Standalone build output is not packageable."""


def validate_release0_client_config(
    config_path: Path | str,
    *,
    allow_local_meta_url: bool,
) -> None:
    path = Path(config_path)
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise PackageError(f"invalid Release0ClientConfig.json: {error}") from error

    if "inviteCode" in data:
        raise PackageError("Release0ClientConfig.json must not contain inviteCode")

    meta_base_url = str(data.get("metaBaseUrl", "")).strip()
    parsed = urlparse(meta_base_url)
    hostname = (parsed.hostname or "").lower()
    local_hosts = {"127.0.0.1", "::1", "localhost"}
    if allow_local_meta_url and hostname in local_hosts:
        return
    if hostname in local_hosts:
        raise PackageError(
            "Release0ClientConfig.json must use a public HTTPS metaBaseUrl"
        )
    if parsed.scheme != "https" or not parsed.netloc:
        raise PackageError(
            "Release0ClientConfig.json must use a public HTTPS metaBaseUrl"
        )


def default_artifacts_dir(repo_root: Path | str | None = None) -> Path:
    root = Path.cwd() if repo_root is None else Path(repo_root)
    return root / "artifacts" / "release0"


def default_build_dir(platform: str, repo_root: Path | str | None = None) -> Path:
    root = Path.cwd() if repo_root is None else Path(repo_root)
    if platform == "windows":
        leaf = "Windows"
    elif platform == "macos":
        leaf = "macOS"
    else:
        raise PackageError(f"unsupported platform: {platform}")
    return (
        root /
        "client" /
        "unity_player_client" /
        "Builds" /
        "Release0" /
        leaf
    )


def package_windows(build_dir: Path | str, output_dir: Path | str) -> Path:
    build_path = Path(build_dir)
    _require_file(build_path / WINDOWS_EXE_NAME)
    _require_file(build_path / "UnityPlayer.dll")
    _require_dir(build_path / WINDOWS_DATA_DIR_NAME)
    config_path = build_path / WINDOWS_RELEASE0_CLIENT_CONFIG_PATH
    _require_file(config_path)
    validate_release0_client_config(config_path, allow_local_meta_url=False)
    return _write_zip(build_path, Path(output_dir), WINDOWS_PACKAGE_NAME)


def package_macos(build_dir: Path | str, output_dir: Path | str) -> Path:
    build_path = Path(build_dir)
    _require_dir(build_path / MACOS_APP_NAME)
    config_path = build_path / MACOS_RELEASE0_CLIENT_CONFIG_PATH
    _require_file(config_path)
    validate_release0_client_config(config_path, allow_local_meta_url=False)
    return _write_zip(build_path, Path(output_dir), MACOS_PACKAGE_NAME)


def _require_file(path: Path) -> None:
    if not path.is_file():
        raise PackageError(f"required file is missing: {path.name}")


def _require_dir(path: Path) -> None:
    if not path.is_dir():
        raise PackageError(f"required directory is missing: {path.name}")


def _write_zip(build_dir: Path, output_dir: Path, package_name: str) -> Path:
    output_dir.mkdir(parents=True, exist_ok=True)
    package_path = output_dir / f"{package_name}.zip"
    package_root = Path(package_name)

    with zipfile.ZipFile(package_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(str(package_root / PACKAGE_README_NAME), PACKAGE_README)
        for path in sorted(build_dir.rglob("*")):
            if not path.is_file():
                continue
            archive.write(path, package_root / path.relative_to(build_dir))

    return package_path


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Package Release 0 Unity Standalone build outputs."
    )
    parser.add_argument(
        "platform",
        choices=("windows", "macos"),
        help="Standalone build platform to package.",
    )
    parser.add_argument(
        "--repo-root",
        default=".",
        help="Repository root used for default build/output paths.",
    )
    parser.add_argument(
        "--build-dir",
        default=None,
        help="Build directory. Defaults to client/unity_player_client/Builds/Release0/<platform>.",
    )
    parser.add_argument(
        "--output-dir",
        default=None,
        help="Package output directory. Defaults to artifacts/release0.",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    repo_root = Path(args.repo_root)
    output_dir = Path(args.output_dir) if args.output_dir else default_artifacts_dir(repo_root)
    build_dir = (
        Path(args.build_dir)
        if args.build_dir
        else default_build_dir(args.platform, repo_root)
    )

    try:
        if args.platform == "windows":
            package_path = package_windows(build_dir, output_dir)
        else:
            package_path = package_macos(build_dir, output_dir)
    except PackageError as error:
        print(f"package failed: {error}", file=sys.stderr)
        return 2

    print(package_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
