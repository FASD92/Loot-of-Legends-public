import tempfile
import unittest
import zipfile
from pathlib import Path

from scripts.release0.unity_standalone_package import (
    PACKAGE_README_NAME,
    PackageError,
    default_artifacts_dir,
    package_macos,
    package_windows,
    validate_release0_client_config,
)


class UnityStandalonePackageTests(unittest.TestCase):
    def test_default_output_goes_under_artifacts_release0(self):
        repo_root = Path("/repo/root")

        self.assertEqual(
            repo_root / "artifacts" / "release0",
            default_artifacts_dir(repo_root),
        )

    def test_windows_package_requires_exe_unity_player_and_data_directory(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir) / "Windows"
            build_dir.mkdir()
            (build_dir / "LootOfLegendsRelease0.exe").write_bytes(b"exe")
            (build_dir / "LootOfLegendsRelease0_Data").mkdir()

            with self.assertRaisesRegex(PackageError, "UnityPlayer.dll"):
                package_windows(build_dir, Path(temp_dir) / "artifacts")

    def test_windows_package_requires_release0_config(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir) / "Windows"
            build_dir.mkdir()
            (build_dir / "LootOfLegendsRelease0.exe").write_bytes(b"exe")
            (build_dir / "UnityPlayer.dll").write_bytes(b"dll")
            data_dir = build_dir / "LootOfLegendsRelease0_Data"
            data_dir.mkdir()

            with self.assertRaisesRegex(PackageError, "Release0ClientConfig.json"):
                package_windows(build_dir, Path(temp_dir) / "artifacts")

    def test_windows_package_contains_runtime_files_and_readme(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir) / "Windows"
            output_dir = Path(temp_dir) / "artifacts"
            build_dir.mkdir()
            (build_dir / "LootOfLegendsRelease0.exe").write_bytes(b"exe")
            (build_dir / "UnityPlayer.dll").write_bytes(b"dll")
            data_dir = build_dir / "LootOfLegendsRelease0_Data"
            data_dir.mkdir()
            (data_dir / "boot.config").write_text("gfx-enable-gfx-jobs=1\n")
            streaming_assets_dir = data_dir / "StreamingAssets"
            streaming_assets_dir.mkdir()
            (streaming_assets_dir / "Release0ClientConfig.json").write_text(
                '{ "metaBaseUrl": "https://release0.example.com" }\n'
            )

            package_path = package_windows(build_dir, output_dir)

            self.assertEqual(
                output_dir / "LootOfLegendsRelease0-Windows.zip",
                package_path,
            )
            with zipfile.ZipFile(package_path) as archive:
                names = set(archive.namelist())
                self.assertIn(
                    "LootOfLegendsRelease0-Windows/LootOfLegendsRelease0.exe",
                    names,
                )
                self.assertIn(
                    "LootOfLegendsRelease0-Windows/UnityPlayer.dll",
                    names,
                )
                self.assertIn(
                    "LootOfLegendsRelease0-Windows/"
                    "LootOfLegendsRelease0_Data/boot.config",
                    names,
                )
                self.assertIn(
                    "LootOfLegendsRelease0-Windows/"
                    "LootOfLegendsRelease0_Data/StreamingAssets/"
                    "Release0ClientConfig.json",
                    names,
                )
                readme = archive.read(
                    f"LootOfLegendsRelease0-Windows/{PACKAGE_README_NAME}"
                ).decode("utf-8")

            self.assertIn("real Google OAuth", readme)
            self.assertIn("local dev-auth is not portfolio evidence", readme)

    def test_release_package_rejects_local_meta_url_by_default(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            config = Path(temp_dir) / "Release0ClientConfig.json"
            config.write_text('{ "metaBaseUrl": "http://127.0.0.1:8080" }\n')

            with self.assertRaisesRegex(PackageError, "public HTTPS"):
                validate_release0_client_config(config, allow_local_meta_url=False)

    def test_release_package_rejects_https_local_meta_url_by_default(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            config = Path(temp_dir) / "Release0ClientConfig.json"
            config.write_text('{ "metaBaseUrl": "https://localhost:8080" }\n')

            with self.assertRaisesRegex(PackageError, "public HTTPS"):
                validate_release0_client_config(config, allow_local_meta_url=False)

    def test_release_package_accepts_public_https_meta_url(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            config = Path(temp_dir) / "Release0ClientConfig.json"
            config.write_text(
                '{ "metaBaseUrl": "https://release0.example.com" }\n'
            )

            validate_release0_client_config(config, allow_local_meta_url=False)

    def test_release_package_rejects_invite_code_in_client_config(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            config = Path(temp_dir) / "Release0ClientConfig.json"
            config.write_text(
                '{ "metaBaseUrl": "https://release0.example.com", '
                '"inviteCode": "portfolio-2026" }\n'
            )

            with self.assertRaisesRegex(PackageError, "inviteCode"):
                validate_release0_client_config(config, allow_local_meta_url=False)

    def test_macos_package_requires_app_bundle(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir) / "macOS"
            build_dir.mkdir()

            with self.assertRaisesRegex(PackageError, "LootOfLegendsRelease0.app"):
                package_macos(build_dir, Path(temp_dir) / "artifacts")

    def test_macos_package_requires_release0_config(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir) / "macOS"
            executable = (
                build_dir /
                "LootOfLegendsRelease0.app" /
                "Contents" /
                "MacOS" /
                "LootOfLegendsRelease0"
            )
            executable.parent.mkdir(parents=True)
            executable.write_bytes(b"bin")

            with self.assertRaisesRegex(PackageError, "Release0ClientConfig.json"):
                package_macos(build_dir, Path(temp_dir) / "artifacts")

    def test_macos_package_contains_app_bundle_and_readme(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            build_dir = Path(temp_dir) / "macOS"
            output_dir = Path(temp_dir) / "artifacts"
            executable = (
                build_dir /
                "LootOfLegendsRelease0.app" /
                "Contents" /
                "MacOS" /
                "LootOfLegendsRelease0"
            )
            executable.parent.mkdir(parents=True)
            executable.write_bytes(b"bin")
            streaming_assets_dir = (
                build_dir /
                "LootOfLegendsRelease0.app" /
                "Contents" /
                "Resources" /
                "Data" /
                "StreamingAssets"
            )
            streaming_assets_dir.mkdir(parents=True)
            (streaming_assets_dir / "Release0ClientConfig.json").write_text(
                '{ "metaBaseUrl": "https://release0.example.com" }\n'
            )

            package_path = package_macos(build_dir, output_dir)

            self.assertEqual(
                output_dir / "LootOfLegendsRelease0-macOS.zip",
                package_path,
            )
            with zipfile.ZipFile(package_path) as archive:
                names = set(archive.namelist())
                self.assertIn(
                    "LootOfLegendsRelease0-macOS/"
                    "LootOfLegendsRelease0.app/"
                    "Contents/MacOS/LootOfLegendsRelease0",
                    names,
                )
                self.assertIn(
                    "LootOfLegendsRelease0-macOS/"
                    "LootOfLegendsRelease0.app/"
                    "Contents/Resources/Data/StreamingAssets/"
                    "Release0ClientConfig.json",
                    names,
                )
                readme = archive.read(
                    f"LootOfLegendsRelease0-macOS/{PACKAGE_README_NAME}"
                ).decode("utf-8")

            self.assertIn("real Google OAuth", readme)
            self.assertIn("local dev-auth is not portfolio evidence", readme)


if __name__ == "__main__":
    unittest.main()
